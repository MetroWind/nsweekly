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
    DO_OR_RETURN(data_source->db->execute(
        "CREATE TABLE IF NOT EXISTS Users "
        "(id INTEGER PRIMARY KEY ASC, name TEXT UNIQUE);"));
    DO_OR_RETURN(data_source->db->execute(
        "CREATE TABLE IF NOT EXISTS Weeklies "
        "(user_id INTEGER REFERENCES Users (id) ON DELETE CASCADE,"
        " week_start INTEGER, update_time INTEGER, content TEXT);"));
    return data_source;
}

E<std::unique_ptr<DataSourceSqlite>> DataSourceSqlite::newFromMemory()
{
    return fromFile(":memory:");
}

std::vector<WeeklyPost>
DataSourceSqlite::getYearOfWeeklies([[maybe_unused]] const std::string& username)
{
    return {};
}

E<std::optional<int64_t>> DataSourceSqlite::getUserID(const std::string& name) const
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr("SELECT id FROM Users WHERE name = ?;"));
    sql.bind(name);
    ASSIGN_OR_RETURN(std::vector<std::tuple<int64_t>> result,
                     db->eval<int64_t>(std::move(sql)));
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

E<void> DataSourceSqlite::createUser(const std::string& name) const
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(
        "INSERT INTO Users (name) VALUES (?);"));
    sql.bind(name);
    DO_OR_RETURN(db->execute(std::move(sql)));
    return {};
}
