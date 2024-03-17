#include <string_view>
#include <memory>
#include <vector>
#include <expected>
#include <tuple>
#include <optional>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include "database.hpp"
#include "error.hpp"

void SQLite::clear()
{
    if(db != nullptr)
    {
        sqlite3_close(db);
        db = nullptr;
    }
}

E<std::unique_ptr<SQLite>>
SQLite::connectFile(const std::string& db_file)
{
    auto data = std::make_unique<SQLite>();
    if(int code = sqlite3_open(db_file.c_str(), &data->db);
       code != SQLITE_OK)
    {
        data->clear();
        return std::unexpected(runtimeError(std::string(
            "Failed to create DB connection: ") + sqlite3_errstr(code)));
    }
    return data;
}

E<std::unique_ptr<SQLite>> SQLite::connectMemory()
{
    return connectFile(":memory:");
}

E<void> SQLite::execute(const char* sql_code) const
{
    ASSIGN_OR_RETURN(auto sql, internal::SQLiteStatement::fromStr(db, sql_code));
    int code = sqlite3_step(sql.data());
    switch(code)
    {
    case SQLITE_DONE:
        return {};
    case SQLITE_BUSY:
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return std::unexpected(runtimeError(sqlite3_errstr(code)));
    case SQLITE_ERROR:
    case SQLITE_MISUSE:
        return std::unexpected(runtimeError(std::string(
            "Failed to execute SQL: ") + sqlite3_errstr(code)));
    default:
        return std::unexpected(runtimeError(std::string(
            "Unexpected return code when executing SQL: ") +
            sqlite3_errstr(code)));
    }
}

SQLite::~SQLite()
{
    clear();
}
