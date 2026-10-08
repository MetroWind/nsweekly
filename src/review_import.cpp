#include <algorithm>
#include <map>
#include <set>
#include <mw/utils.hpp>
#include "csv_reader.hpp"
#include "game_import.hpp"

namespace
{
struct ReviewImportRow
{
    size_t number;
    int64_t game_id;
    GameReviewInput input;
    int64_t added;
    int64_t updated;
};

bool blankReviewRecord(const CsvRecord &record)
{
    for(const auto &value : record.fields)
    {
        if(!mw::strip(value).empty())
        {
            return false;
        }
    }
    return true;
}

int64_t reviewDateTimestamp(std::chrono::year_month_day date)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::sys_days(date).time_since_epoch()).count();
}

void reviewImportSummary(std::ostream &output, size_t input, size_t blank,
    size_t inserted, size_t file_duplicates, size_t db_duplicates)
{
    output << "input=" << input << " blank=" << blank
           << " inserted=" << inserted
           << " duplicate-in-file=" << file_duplicates
           << " duplicate-in-database=" << db_duplicates << '\n';
}

void reviewImportErrors(std::ostream &output, const CsvRecord &record,
                         const Error &error)
{
    if(const auto *validation = error.as<GameValidationError>())
    {
        for(const auto &[field, message] : validation->fields)
        {
            const auto column = field == "start_date" ? "Addition" :
                field == "end_date" ? "Update" :
                field == "name" ? "Game" : field;
            output << "Record " << record.number << ", line " << record.line
                   << ", " << column << ": " << message << '\n';
        }
    }
    else
    {
        output << "Record " << record.number << ", line " << record.line
               << ": " << errorMsg(error) << '\n';
    }
}
} // namespace

int importReviewsCsv(const std::string &text, const std::string &user,
                     GameDataInterface &games, std::ostream &output)
{
    auto parsed = readCsv(text);
    if(!parsed || parsed->empty())
    {
        output << (parsed ? "CSV header is missing" : errorMsg(parsed.error()))
               << '\n';
        return 2;
    }
    const std::map<std::string, std::string> mapping{
        {"Story/Lore", "story"}, {"Game Play", "gameplay"},
        {"Graphics", "graphics"}, {"Audio", "audio"},
        {"Special", "special"}};
    const auto &header = parsed->front();
    std::map<std::string, size_t> columns;
    for(size_t i = 0; i < header.fields.size(); ++i)
    {
        std::string name(mw::strip(header.fields[i]));
        if(name == "Name")
        {
            name = "Game";
        }
        if(name == "Gameplay")
        {
            name = "Game Play";
        }
        if(name.empty())
        {
            output << "Ignored unnamed column " << i + 1 << '\n';
            continue;
        }
        if(!columns.emplace(name, i).second)
        {
            output << "Duplicate CSV header: " << name << '\n';
            return 2;
        }
        if(!mapping.contains(name) && name != "Game" &&
           name != "Addition" && name != "Update")
        {
            output << "Ignored column: " << name << '\n';
        }
    }
    for(const auto &name : {"Game", "Story/Lore", "Game Play", "Graphics",
                             "Audio", "Special"})
    {
        if(!columns.contains(name))
        {
            output << "Required CSV header: " << name << '\n';
            return 2;
        }
    }
    auto existing = games.listGames(user);
    if(!existing)
    {
        output << "Storage read failed\n";
        return 4;
    }
    std::map<std::string, int64_t> game_ids;
    for(const auto &game : *existing)
    {
        game_ids.emplace(game.input.name, game.id);
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    size_t blank = 0;
    size_t duplicates = 0;
    bool invalid = false;
    std::set<int64_t> seen;
    std::vector<ReviewImportRow> inputs;
    for(size_t i = 1; i < parsed->size(); ++i)
    {
        const auto &record = (*parsed)[i];
        if(blankReviewRecord(record))
        {
            ++blank;
            continue;
        }
        if(record.fields.size() != header.fields.size())
        {
            output << "Record " << record.number << ", line " << record.line
                   << ": wrong field count\n";
            invalid = true;
            continue;
        }
        GameFields identity;
        identity.scalars = {{"name", record.fields[columns.at("Game")]},
                            {"status", "now_playing"}};
        if(columns.contains("Addition"))
        {
            identity.scalars["start_date"] =
                record.fields[columns.at("Addition")];
        }
        if(columns.contains("Update"))
        {
            identity.scalars["end_date"] = record.fields[columns.at("Update")];
        }
        auto parent = validateGameInput(identity);
        GameFields fields;
        for(const auto &[column, key] : mapping)
        {
            if(columns.contains(column))
            {
                const auto &value = record.fields[columns.at(column)];
                fields.scalars[key] = std::string(mw::strip(value));
            }
        }
        auto input = validateReview(fields);
        if(!parent || !input)
        {
            invalid = true;
            if(!parent)
            {
                reviewImportErrors(output, record, parent.error());
            }
            if(!input)
            {
                reviewImportErrors(output, record, input.error());
            }
            continue;
        }
        if(parent->end_date && !parent->start_date)
        {
            output << "Record " << record.number << ", line " << record.line
                   << ": Update requires Addition\n";
            invalid = true;
            continue;
        }
        auto match = game_ids.find(parent->name);
        if(match == game_ids.end())
        {
            output << "Record " << record.number << ", line " << record.line
                   << ": Game not found in target tracker: "
                   << parent->name << '\n';
            invalid = true;
            continue;
        }
        if(!seen.insert(match->second).second)
        {
            ++duplicates;
            output << "Duplicate record " << record.number << '\n';
            continue;
        }
        const int64_t added = parent->start_date ?
            reviewDateTimestamp(*parent->start_date) : now;
        const int64_t updated = parent->end_date ?
            reviewDateTimestamp(*parent->end_date) : added;
        inputs.push_back({record.number, match->second, std::move(*input),
                          added, updated});
    }
    if(invalid)
    {
        output << "Validation failed; no reviews inserted\n";
        return 2;
    }
    size_t inserted = 0;
    size_t db_duplicates = 0;
    for(const auto &row : inputs)
    {
        auto saved = games.importReview(user, row.game_id, row.input,
                                        row.added, row.updated);
        if(!saved)
        {
            output << "Storage failure at record " << row.number << "; "
                   << inserted << " inserts committed\n";
            reviewImportSummary(output, parsed->size() - 1, blank,
                inserted, duplicates, db_duplicates);
            return 4;
        }
        if(*saved)
        {
            ++inserted;
        }
        else
        {
            ++db_duplicates;
        }
    }
    reviewImportSummary(output, parsed->size() - 1, blank,
                         inserted, duplicates, db_duplicates);
    return 0;
}
