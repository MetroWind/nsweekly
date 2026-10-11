#pragma once
#include "attachment/attachment.hpp"
#include "database.hpp"

// Dedicated connection; its owner serializes all access with the file mutex.
class AttachmentDataSqlite
{
public:
    // Takes an isolated connection to the application's existing database.
    explicit AttachmentDataSqlite(std::unique_ptr<SQLite> connection)
        : db(std::move(connection))
    {
    }
    // Creates additive, idempotent attachment tables after Users.
    E<void> initializeSchema();
    // Returns owner-scoped records newest first.
    E<std::vector<Attachment>> list(int64_t user);
    // Verifies that a storage caller's owner exists.
    E<void> owner(int64_t user);
    // Connection used only by the coordinating service under its mutex.
    std::unique_ptr<SQLite> db;
};
// Rolls back an incomplete attachment metadata mutation.
class AttachmentTransaction
{
public:
    // Borrows a connection already protected by the service mutex.
    explicit AttachmentTransaction(SQLite& connection) : db(connection) {}
    // Starts a write transaction.
    E<void> begin();
    // Makes logical changes durable.
    E<void> commit();
    // Rolls back on every error path.
    ~AttachmentTransaction();
    AttachmentTransaction(const AttachmentTransaction&) = delete;
    AttachmentTransaction& operator=(const AttachmentTransaction&) = delete;

private:
    SQLite& db;
    bool active = false;
};
