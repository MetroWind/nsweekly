#pragma once
#include "database.hpp"
#include "weekly.hpp"

// Describes storage operations for weekly snippets.
class WeeklyDataInterface
{
public:
    // Allows destruction through the storage interface.
    virtual ~WeeklyDataInterface() = default;
    // Returns posts and empty Mondays in ascending week order, [begin, end).
    virtual E<std::vector<WeeklyPost>> getWeeklies(
        const std::string& user, const Time& begin,
        const Time& end) const = 0;
    // Creates or updates a weekly, ensuring its author exists first.
    virtual E<void> updateWeekly(
        const std::string& username, WeeklyPost&& new_post) const = 0;
    // Retrieves the existing rolling one-year range.
    E<std::vector<WeeklyPost>> getWeekliesOneYear(
        const std::string& user) const;
};

// Implements weekly storage using its own SQLite connection.
class WeeklyDataSqlite : public WeeklyDataInterface
{
public:
    // Takes ownership of a configured connection to the shared database.
    explicit WeeklyDataSqlite(std::unique_ptr<SQLite> conn)
        : db(std::move(conn)) {}
    // Creates the existing Weeklies table after Users has been initialized.
    E<void> initializeSchema();
    // Returns posts and placeholders for the half-open range.
    E<std::vector<WeeklyPost>> getWeeklies(
        const std::string& user, const Time& begin,
        const Time& end) const override;
    // Upserts a Monday post, ensuring the author on this connection first.
    E<void> updateWeekly(const std::string& username,
                         WeeklyPost&& new_post) const override;
private:
    std::unique_ptr<SQLite> db;
};
