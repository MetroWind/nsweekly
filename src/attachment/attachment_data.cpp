#include "attachment/attachment_data.hpp"
#include <spdlog/spdlog.h>

E<void> AttachmentDataSqlite::initializeSchema()
{
    DO_OR_RETURN(db->execute("PRAGMA foreign_keys=ON"));
    ASSIGN_OR_RETURN(auto enabled, db->eval<int>("PRAGMA foreign_keys"));
    if(enabled.empty() || std::get<0>(enabled[0]) != 1)
    {
        return std::unexpected(
            runtimeError("Attachment foreign keys unavailable"));
    }
    AttachmentTransaction transaction(*db);
    DO_OR_RETURN(transaction.begin());
    const std::string schema = R"(
CREATE TABLE IF NOT EXISTS AttachmentFiles(
 hash TEXT PRIMARY KEY CHECK(length(hash)=32 AND
 hash NOT GLOB '*[^0-9a-f]*'),
 byte_size INTEGER NOT NULL CHECK(byte_size>=0),
 mime_type TEXT NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS Attachments(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 user_id INTEGER NOT NULL REFERENCES Users(id) ON DELETE RESTRICT,
 file_hash TEXT NOT NULL REFERENCES AttachmentFiles(hash) ON DELETE RESTRICT,
 original_name TEXT COLLATE BINARY NOT NULL, added INTEGER NOT NULL,
 UNIQUE(user_id,file_hash,original_name));
CREATE INDEX IF NOT EXISTS AttachmentSource ON Attachments(file_hash);
CREATE TABLE IF NOT EXISTS AttachmentVariants(
 source_hash TEXT NOT NULL REFERENCES AttachmentFiles(hash) ON DELETE RESTRICT,
 recipe TEXT NOT NULL,
 output_hash TEXT NOT NULL REFERENCES AttachmentFiles(hash) ON DELETE RESTRICT,
 width INTEGER NOT NULL CHECK(width>0),
 height INTEGER NOT NULL CHECK(height>0), format TEXT NOT NULL,
 created INTEGER NOT NULL, PRIMARY KEY(source_hash,recipe));
CREATE INDEX IF NOT EXISTS AttachmentOutput ON AttachmentVariants(output_hash);
CREATE TABLE IF NOT EXISTS AttachmentCleanup(
 hash TEXT PRIMARY KEY REFERENCES AttachmentFiles(hash) ON DELETE RESTRICT);
)";
    size_t offset = 0;
    while(offset < schema.size())
    {
        auto end = schema.find(';', offset);
        if(end == schema.npos)
        {
            break;
        }
        DO_OR_RETURN(db->execute(schema.substr(offset, end - offset + 1)));
        offset = end + 1;
    }
    return transaction.commit();
}

E<void> AttachmentDataSqlite::owner(int64_t user)
{
    ASSIGN_OR_RETURN(auto sql,
                     db->statementFromStr("SELECT id FROM Users WHERE id=?"));
    DO_OR_RETURN(sql.bind(user));
    ASSIGN_OR_RETURN(auto rows, db->eval<int64_t>(std::move(sql)));
    if(rows.empty())
    {
        return std::unexpected(httpError(404, "Unknown owner"));
    }
    return {};
}

E<std::vector<Attachment>> AttachmentDataSqlite::list(int64_t user)
{
    DO_OR_RETURN(owner(user));
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
SELECT a.id,a.user_id,a.added,a.original_name,f.hash,f.mime_type,
 f.byte_size,f.created FROM Attachments a JOIN AttachmentFiles f
 ON a.file_hash=f.hash WHERE a.user_id=? ORDER BY a.added DESC,a.id DESC
)"));
    DO_OR_RETURN(sql.bind(user));
    ASSIGN_OR_RETURN(
        auto rows,
        (db->eval<int64_t, int64_t, int64_t, std::string, std::string,
                  std::string, int64_t, int64_t>(std::move(sql))));
    std::vector<Attachment> result;
    for(auto& [id, owner_id, added, name, hash, mime, size, created] : rows)
    {
        result.push_back(
            {id, owner_id, added, name, {hash, mime, size, created}});
    }
    return result;
}

E<void> AttachmentTransaction::begin()
{
    DO_OR_RETURN(db.execute("BEGIN IMMEDIATE"));
    active = true;
    return {};
}
E<void> AttachmentTransaction::commit()
{
    DO_OR_RETURN(db.execute("COMMIT"));
    active = false;
    return {};
}
AttachmentTransaction::~AttachmentTransaction()
{
    if(active && !db.execute("ROLLBACK"))
    {
        spdlog::error("Attachment rollback failed");
        if(!db.autocommit())
        {
            db.invalidate();
        }
    }
}
