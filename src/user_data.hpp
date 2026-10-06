#pragma once
#include "database.hpp"

// Describes shared user storage independently of feature storage.
class UserDataInterface
{
public:
    // Allows destruction through the storage interface.
    virtual ~UserDataInterface() = default;
    // Retrieves an ID, returning no value when the name is absent.
    virtual E<std::optional<int64_t>> getUserID(
        const std::string& name) const = 0;
    // Creates a user, failing if its name already exists.
    virtual E<int64_t> createUser(const std::string& name) const = 0;
    // Ensures a user exists, including when another request creates it.
    virtual E<void> ensureUser(const std::string& name) const = 0;
};

// Implements shared users with its own SQLite connection.
class UserDataSqlite : public UserDataInterface
{
public:
    // Takes ownership of a configured SQLite connection.
    explicit UserDataSqlite(std::unique_ptr<SQLite> conn)
        : db(std::move(conn)) {}
    // Creates the existing Users table if absent.
    E<void> initializeSchema();
    // Returns the ID for an existing user.
    E<std::optional<int64_t>> getUserID(
        const std::string& name) const override;
    // Returns the ID of a newly inserted user.
    E<int64_t> createUser(const std::string& name) const override;
    // Idempotently creates the named user.
    E<void> ensureUser(const std::string& name) const override;
private:
    std::unique_ptr<SQLite> db;
};
