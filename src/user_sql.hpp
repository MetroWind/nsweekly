#pragma once
#include <optional>
#include "database.hpp"

// Creates the shared Users schema on a supplied connection.
E<void> initializeUsers(SQLite& db);
// Looks up a user without treating an absent name as an error.
E<std::optional<int64_t>> getUserID(SQLite& db, const std::string& name);
// Inserts a new user and returns its statement-local ID.
E<int64_t> createUser(SQLite& db, const std::string& name);
// Ensures a name exists without failing on an existing user.
E<void> ensureUser(SQLite& db, const std::string& name);
