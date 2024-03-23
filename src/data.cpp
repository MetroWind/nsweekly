#include <iterator>
#include <string_view>
#include <memory>
#include <vector>
#include <expected>
#include <tuple>
#include <optional>
#include <chrono>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include "data.hpp"
#include "database.hpp"
#include "error.hpp"
#include "utils.hpp"
#include "weekly.hpp"

namespace
{

// Calculate all the Monday 00:00 in a time period
std::vector<Time> allWeekStarts(const Time& begin, const Time& end)
{
    std::chrono::weekday w(std::chrono::floor<std::chrono::days>(begin));
    int diff = w.iso_encoding() - 1;
    Time mon = begin;
    if(diff != 0)
    {
        mon += std::chrono::days(7 - diff);
    }
    std::vector<Time> result;
    while(mon < end)
    {
        result.push_back(mon);
        mon += std::chrono::days(7);
    }
    return result;
}

} // namespace


E<std::vector<WeeklyPost>>
DataSourceInterface::getWeekliesOneYear(const std::string& user) const
{
    auto now = Clock::now();
    return this->getWeeklies(user, now - std::chrono::years(1), now);
}

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
        " week_start INTEGER, update_time INTEGER, format INTEGER,"
        " lang TEXT, content TEXT, UNIQUE (user_id, week_start));"));
    return data_source;
}

E<std::unique_ptr<DataSourceSqlite>> DataSourceSqlite::newFromMemory()
{
    return fromFile(":memory:");
}

E<std::vector<WeeklyPost>> DataSourceSqlite::getWeeklies(
    const std::string& username, const Time& begin, const Time& end) const
{
    ASSIGN_OR_RETURN(std::optional<int64_t> uid, getUserID(username));
    if(!uid.has_value())
    {
        return std::unexpected(runtimeError("User not found"));
    }
    int64_t start = timeToSeconds(begin);
    int64_t stop = timeToSeconds(end);
    // Get all rows whose week_start is in the time period.
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(
        "SELECT content, format, lang, week_start, update_time FROM Weeklies "
        "WHERE user_id = ? AND week_start >= ? AND week_start < ?;"));
    DO_OR_RETURN(sql.bind(*uid, start, stop));
    ASSIGN_OR_RETURN(
        auto rows, (db->eval<std::string, int, std::string, int64_t, int64_t>(
            std::move(sql))));
    // Converting rows to weekly objects.
    std::vector<WeeklyPost> weeklies;
    weeklies.reserve(rows.size());
    for(auto& row: rows)
    {
        int format = std::get<1>(row);
        if(!WeeklyPost::isValidFormatInt(format))
        {
            return std::unexpected(runtimeError(std::format(
                "Invalid format: {}", format)));
        }
        WeeklyPost p;
        p.format = static_cast<WeeklyPost::Format>(format);
        p.raw_content = std::move(std::get<0>(row));
        p.week_begin = secondsToTime(std::get<3>(row));
        p.update_time = secondsToTime(std::get<4>(row));
        p.language = std::move(std::get<2>(row));
        weeklies.push_back(std::move(p));
    }

    // Now we have a set of weeklies, the week_begin of each of which
    // is a Monday. However, these Mondays are only a subset of all
    // the Mondays in the queried time period. Our goal is to return
    // one weekly for each of the Monday in the time period. If there
    // is not a weekly on a Monday, we return an empty weekly for
    // that. This way the logic in the HTTP handler and the frontend
    // is simplified.
    //
    // Our algorithm for this is the classic double pointer. We will
    // have a “pointer” for all the mondays in the time peroid, and
    // another for the (non-empty) weeklies, both from the start of
    // the respective sequence. If the current weekly’s week_begin
    // happens to be the current monday, we add this weekly to the
    // result, and advance both pointers; if not we just advance the
    // Monday pointer.
    //
    // I think this could be an interview question...
    std::vector<Time> week_starts = allWeekStarts(begin, end);
    std::vector<WeeklyPost> result(week_starts.size());
    // The monday “pointer”
    auto monday_it = std::begin(week_starts);
    // The weeklies “pointer”
    auto weekly_it = std::begin(weeklies);
    auto result_it = std::begin(result); // This should sync with monday_it.

    while(true)
    {
        if(weekly_it != std::end(weeklies) &&
           weekly_it->week_begin == *monday_it)
        {
            *result_it = *weekly_it;
            ++monday_it;
            ++weekly_it;
            ++result_it;
        }
        else
        {
            result_it->week_begin = *monday_it;
            ++monday_it;
            ++result_it;
        }
        if(monday_it == std::end(week_starts))
        {
            break;
        }
    }

    return result;
}

E<void> DataSourceSqlite::updateWeekly(
    const std::string& username, WeeklyPost&& new_post) const
{
    ASSIGN_OR_RETURN(std::optional<int64_t> uid, getUserID(username));
    if(!uid.has_value())
    {
        ASSIGN_OR_RETURN(uid, createUser(username));
    }
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(
        "INSERT INTO weeklies "
        "(user_id, week_start, update_time, format, lang, content) "
        "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT DO UPDATE SET update_time = ?, "
        "format = ?, lang = ?, content = ?;"));
    int64_t now = timeToSeconds(Clock::now());
    DO_OR_RETURN(sql.bind(
        *uid, timeToSeconds(new_post.week_begin), now,
        static_cast<int>(new_post.format), new_post.language,
        new_post.raw_content, now, static_cast<int>(new_post.format),
        new_post.language, new_post.raw_content));
    return db->execute(std::move(sql));
}

E<std::optional<int64_t>>
DataSourceSqlite::getUserID(const std::string& name) const
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(
        "SELECT id FROM Users WHERE name = ?;"));
    DO_OR_RETURN(sql.bind(name));
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

E<int64_t> DataSourceSqlite::createUser(const std::string& name) const
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(
        "INSERT INTO Users (name) VALUES (?);"));
    DO_OR_RETURN(sql.bind(name));
    DO_OR_RETURN(db->execute(std::move(sql)));
    return db->lastInsertRowID();
}
