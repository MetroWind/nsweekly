#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <mw/utils.hpp>
#include "csv_reader.hpp"
#include "game_import.hpp"
#include "user_data.hpp"

namespace
{
bool blankRecord(const CsvRecord &record)
{
    return std::ranges::all_of(record.fields,
                               [](const std::string &value)
                               {
                                   return mw::strip(value).empty();
                               });
}

template <typename T, size_t N>
std::string labelCode(const std::array<GameChoice<T>, N> &choices,
                      const std::string &value)
{
    auto stripped = mw::strip(value);
    if(stripped.empty())
    {
        return "";
    }
    for(const auto &choice : choices)
    {
        if(choice.label == stripped)
        {
            return std::string(choice.code);
        }
    }
    return "invalid:" + std::string(stripped);
}

void summary(std::ostream &output, size_t input, size_t blank, size_t inserted,
             size_t file_duplicates, size_t db_duplicates)
{
    output << "input=" << input << " blank=" << blank
           << " inserted=" << inserted
           << " duplicate-in-file=" << file_duplicates
           << " duplicate-in-database=" << db_duplicates << '\n';
}
} // namespace

int importGamesCsv(const std::string &text, const std::string &user,
                   GameDataInterface &games, std::ostream &output)
{
    auto parsed = readCsv(text);
    if(!parsed)
    {
        output << errorMsg(parsed.error()) << '\n';
        return 2;
    }
    if(parsed->empty())
    {
        output << "CSV header is missing\n";
        return 2;
    }
    std::map<std::string, size_t> columns;
    const std::map<std::string, std::string> mapping{
        {"Game", "name"},
        {"Status", "status"},
        {"Completion", "completion"},
        {"Hours", "hours"},
        {"Start date", "start_date"},
        {"End date", "end_date"},
        {"Notes", "notes"}};
    const auto &header = parsed->front();
    for(size_t i = 0; i < header.fields.size(); ++i)
    {
        std::string name(mw::strip(header.fields[i]));
        if(name == "Name")
        {
            name = "Game";
        }
        if(!columns.emplace(name, i).second)
        {
            output << "Duplicate CSV header: " << name << '\n';
            return 2;
        }
        if(!mapping.contains(name) && name != "Platform" &&
           name != "2nd Platform")
        {
            output << "Ignored column: " << name << '\n';
        }
    }
    if(!columns.contains("Game") || !columns.contains("Status"))
    {
        output << "Game and Status headers are required\n";
        return 2;
    }
    size_t blank = 0;
    size_t file_duplicates = 0;
    bool invalid = false;
    std::map<std::string, size_t> names;
    std::vector<std::pair<size_t, GameInput>> inputs;
    for(size_t i = 1; i < parsed->size(); ++i)
    {
        const auto &record = (*parsed)[i];
        if(blankRecord(record))
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
        GameFields fields;
        for(const auto &[column, field] : mapping)
        {
            if(columns.contains(column))
            {
                fields.scalars[field] = record.fields[columns.at(column)];
            }
        }
        fields.scalars["status"] =
            labelCode(STATUS_CHOICES, fields.scalars["status"]);
        fields.scalars["completion"] =
            labelCode(COMPLETION_CHOICES, fields.scalars["completion"]);
        for(const auto &column : {"Platform", "2nd Platform"})
        {
            if(!columns.contains(column))
            {
                continue;
            }
            auto code =
                labelCode(PLATFORM_CHOICES, record.fields[columns.at(column)]);
            if(!code.empty())
            {
                fields.platforms.push_back(std::move(code));
            }
        }
        auto input = validateGameInput(fields);
        if(!input)
        {
            invalid = true;
            const auto *error = input.error().as<GameValidationError>();
            for(const auto &[field, message] : error->fields)
            {
                output << "Record " << record.number << ", line " << record.line
                       << ", " << field << ": " << message;
                if(field != "notes")
                {
                    if(field == "platforms")
                    {
                        for(const auto &code : fields.platforms)
                        {
                            output << " [" << code << "]";
                        }
                    }
                    else
                    {
                        output << " [" << fields.scalars[field] << "]";
                    }
                }
                output << '\n';
            }
            continue;
        }
        auto [existing, unique] = names.emplace(input->name, record.number);
        if(!unique)
        {
            ++file_duplicates;
            output << "Duplicate record " << record.number << " (first at "
                   << existing->second << ")\n";
        }
        else
        {
            inputs.emplace_back(record.number, std::move(*input));
        }
    }
    if(invalid)
    {
        output << "Validation failed; no games inserted\n";
        return 2;
    }
    auto existing = games.listGames(user);
    if(!existing)
    {
        output << "Storage read failed\n";
        return 4;
    }
    std::set<std::string> stored_names;
    for(const auto &record : *existing)
    {
        stored_names.insert(record.input.name);
    }
    size_t inserted = 0;
    size_t db_duplicates = 0;
    for(const auto &[number, input] : inputs)
    {
        if(stored_names.contains(input.name))
        {
            ++db_duplicates;
            continue;
        }
        auto saved = games.createGame(user, input);
        if(!saved)
        {
            output << "Storage failure at record " << number << "; " << inserted
                   << " inserts committed\n";
            summary(output, parsed->size() - 1, blank, inserted,
                    file_duplicates, db_duplicates);
            return 4;
        }
        ++inserted;
        stored_names.insert(input.name);
    }
    summary(output, parsed->size() - 1, blank, inserted, file_duplicates,
            db_duplicates);
    return 0;
}

int runGameImport(const Configuration &config, const std::string &file,
                  const std::string &user, std::ostream &output,
                  GameImportKind kind)
{
    std::ifstream stream(file, std::ios::binary);
    if(!stream)
    {
        output << "Cannot read CSV file\n";
        return 2;
    }
    std::string text{std::istreambuf_iterator<char>(stream), {}};
    if(stream.bad())
    {
        output << "CSV read failed\n";
        return 2;
    }
    auto path = (std::filesystem::path(config.data_dir) / "data.db").string();
    auto user_db = SQLite::connectFile(path);
    auto game_db = SQLite::connectFile(path);
    if(!user_db || !game_db)
    {
        output << "Cannot open storage\n";
        return 4;
    }
    UserDataSqlite users(std::move(*user_db));
    GameDataSqlite games(std::move(*game_db));
    if(!users.initializeSchema() || !games.initializeSchema())
    {
        output << "Cannot initialize storage\n";
        return 4;
    }
    auto uid = users.getUserID(user);
    if(!uid)
    {
        output << "User lookup failed\n";
        return 4;
    }
    if(!*uid)
    {
        output << "Unknown target user\n";
        return 2;
    }
    return kind == GameImportKind::REVIEWS ?
        importReviewsCsv(text, user, games, output) :
        importGamesCsv(text, user, games, output);
}
