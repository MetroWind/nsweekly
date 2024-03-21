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
    virtual E<std::vector<WeeklyPost>> getWeeklies(
        const std::string& user, const Time& begin, const Time& end) const = 0;
    virtual E<void> updateWeekly(const std::string& username,
                                 WeeklyPost&& new_post) const = 0;
    virtual E<std::optional<int64_t>> getUserID(const std::string& name) const
    = 0;

    // Convenient function to get weeklies in the last year.
    E<std::vector<WeeklyPost>> getWeekliesOneYear(const std::string& user) const;
};

// Username and start time of the week uniquely identify a weekly
// post.
class DataSourceSqlite : public DataSourceInterface
{
public:
    explicit DataSourceSqlite(std::unique_ptr<SQLite> conn)
            : db(std::move(conn)) {}

    ~DataSourceSqlite() override = default;
    DataSourceSqlite(const DataSourceSqlite&) = delete;
    DataSourceSqlite& operator=(const DataSourceSqlite&) = delete;

    static E<std::unique_ptr<DataSourceSqlite>>
    fromFile(const std::string& db_file);
    static E<std::unique_ptr<DataSourceSqlite>> newFromMemory();

    // Return the weeklies of a user, from begin (inclusive) to end
    // (exclusive).
    E<std::vector<WeeklyPost>> getWeeklies(
        const std::string& user, const Time& begin, const Time& end)
        const override;

    // Update a weekly if exists, otherwise just create the weekly. If
    // user does not exists, create the user first. Caller should make
    // sure that the week_begin time in new_post should be exactly
    // 00:00 UTC on a Monday. This function does not check the
    // validity of week_begin, but failure to do so is undefined
    // behavior.
    E<void> updateWeekly(const std::string& username,
                         WeeklyPost&& new_post) const;
    E<std::optional<int64_t>> getUserID(const std::string& name) const;
    // Do not use.
    DataSourceSqlite() = default;
private:
    // Create a user and return user_id.
    E<int64_t> createUser(const std::string& name) const;

    std::unique_ptr<SQLite> db;
};
