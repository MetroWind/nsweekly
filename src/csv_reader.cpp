#include "csv_reader.hpp"
#include "game.hpp"

E<std::vector<CsvRecord>> readCsv(const std::string &text)
{
    if(!validGameText(text))
    {
        return std::unexpected(
            runtimeError("CSV contains invalid UTF-8 or NUL"));
    }
    enum class State
    {
        START,
        UNQUOTED,
        QUOTED,
        CLOSED
    };
    State state = State::START;
    std::vector<CsvRecord> records;
    CsvRecord record{1, 1, {}};
    std::string field;
    size_t line = 1;
    size_t begin = text.starts_with("\xef\xbb\xbf") ? 3 : 0;
    bool pending = false;
    for(size_t i = begin; i < text.size(); ++i)
    {
        char c = text[i];
        pending = true;
        if(state == State::QUOTED)
        {
            if(c == '"')
            {
                state = State::CLOSED;
            }
            else
            {
                field += c;
                if(c == '\n')
                {
                    ++line;
                }
            }
            continue;
        }
        if(state == State::CLOSED && c == '"')
        {
            field += '"';
            state = State::QUOTED;
            continue;
        }
        if(c == ',' || c == '\n' || c == '\r')
        {
            record.fields.push_back(std::move(field));
            field.clear();
            state = State::START;
            if(c == ',')
            {
                continue;
            }
            if(c == '\r')
            {
                if(i + 1 >= text.size() || text[i + 1] != '\n')
                {
                    return std::unexpected(runtimeError(
                        std::format("CSV record {}, line {}: bare CR",
                                    record.number, line)));
                }
                ++i;
            }
            records.push_back(std::move(record));
            record = CsvRecord{records.size() + 1, ++line, {}};
            pending = false;
            continue;
        }
        if(c == '"' && state == State::START)
        {
            state = State::QUOTED;
            continue;
        }
        if(c == '"' || state == State::CLOSED)
        {
            return std::unexpected(runtimeError(
                std::format("CSV record {}, starting line {}: malformed quote",
                            record.number, record.line)));
        }
        state = State::UNQUOTED;
        field += c;
    }
    if(state == State::QUOTED)
    {
        return std::unexpected(runtimeError(
            std::format("CSV record {}, starting line {}: unterminated quote",
                        record.number, record.line)));
    }
    if(pending)
    {
        record.fields.push_back(std::move(field));
        records.push_back(std::move(record));
    }
    return records;
}
