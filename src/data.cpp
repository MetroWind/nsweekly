#include <string_view>
#include <memory>
#include <vector>
#include <expected>
#include <tuple>
#include <optional>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include "data.hpp"
#include "database.hpp"
#include "error.hpp"
#include "utils.hpp"

E<std::unique_ptr<DataSourceSqlite>>
DataSourceSqlite::fromFile(const std::string& db_file)
{
    auto data_source = std::make_unique<DataSourceSqlite>();
    ASSIGN_OR_RETURN(data_source->db, SQLite::connectFile(db_file));
    auto result = data_source->db->execute(
        "CREATE TABLE IF NOT EXISTS Users "
        "(id INTEGER PRIMARY KEY ASC, name TEXT UNIQUE);");
    if(!result.has_value())
    {
        return std::unexpected(result.error());
    }
    return data_source;
}

E<std::unique_ptr<DataSourceSqlite>> DataSourceSqlite::newFromMemory()
{
    return fromFile(":memory:");
}

std::vector<WeeklyPost>
DataSourceSqlite::getYearOfWeeklies([[maybe_unused]] std::string_view username)
{
    return {};
}


E<std::optional<int64_t>> DataSourceSqlite::getUserID(std::string_view name)
{
    if(name.contains('"'))
    {
        return std::unexpected(runtimeError(
            std::format("Invalid username: {}", name)));
    }

    ASSIGN_OR_RETURN(std::vector<std::tuple<int64_t>> result,
                     db->eval<int64_t>(std::format(
        "SELECT id FROM Users WHERE name = \"{}\";", name)));
    if(result.empty())
    {
        return std::nullopt;
    }
    if(result.size() > 1)
    {
        return std::unexpected(runtimeError(
            std::format("Found multiple IDs for user {}", name)));
    }
    return std::get<0>(result[0]);
}
