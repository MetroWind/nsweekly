#pragma once
#include <memory>
#include <tuple>
#include <vector>
#include <string>
#include <string_view>
#include <optional>

#include <sqlite3.h>

#include "database.hpp"
#include "weekly.hpp"
#include "error.hpp"

class DataSourceInterface
{
public:
    virtual ~DataSourceInterface() = default;
    // Return the weeklies of the last 52 weeks.
    virtual std::vector<WeeklyPost> getYearOfWeeklies(std::string_view username) = 0;
};

class DataSourceSqlite : public DataSourceInterface
{
public:
    DataSourceSqlite() = default;
    ~DataSourceSqlite() override = default;
    DataSourceSqlite(const DataSourceSqlite&) = delete;
    DataSourceSqlite& operator=(const DataSourceSqlite&) = delete;

    static E<std::unique_ptr<DataSourceSqlite>>
    fromFile(const std::string& db_file);
    static E<std::unique_ptr<DataSourceSqlite>> newFromMemory();

    // Return the weeklies of the last 52 weeks.
    std::vector<WeeklyPost> getYearOfWeeklies(std::string_view username)
        override;
    E<std::optional<int64_t>> getUserID(std::string_view name);

private:
    std::unique_ptr<SQLite> db;
};
