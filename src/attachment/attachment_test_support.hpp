#pragma once
#include "attachment/attachment_service.hpp"
#include "storage_test_support.hpp"
#include "user_sql.hpp"

// Builds an isolated attachment dependency for application composition tests.
inline E<std::unique_ptr<AttachmentService>>
testAttachmentService(Configuration config, const TemporaryDatabase& temporary)
{
    config.attachment_dir =
        (std::filesystem::path(temporary.path()).parent_path() / "attachments")
            .string();
    ASSIGN_OR_RETURN(auto connection, SQLite::connectMemory());
    DO_OR_RETURN(initializeUsers(*connection));
    DO_OR_RETURN(ensureUser(*connection, "mw"));
    auto service = std::make_unique<AttachmentService>(
        std::make_unique<AttachmentDataSqlite>(std::move(connection)), config);
    DO_OR_RETURN(service->initialize());
    return service;
}
