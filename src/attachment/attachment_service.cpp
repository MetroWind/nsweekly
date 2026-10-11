#include "attachment/attachment_service.hpp"
#include "attachment/image_variant.hpp"
#include <algorithm>
#include <fcntl.h>
#include <set>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace
{
template <class... Args>
E<void> execute(SQLite& db, const char* text, Args... args)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(text));
    DO_OR_RETURN(sql.bind(args...));
    return db.execute(std::move(sql));
}
E<int64_t> references(SQLite& db, const std::string& hash)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(R"(
SELECT (SELECT count(*) FROM Attachments WHERE file_hash=?) +
 (SELECT count(*) FROM AttachmentVariants WHERE output_hash=? AND source_hash<>?)
)"));
    DO_OR_RETURN(sql.bind(hash, hash, hash));
    ASSIGN_OR_RETURN(auto rows, db.eval<int64_t>(std::move(sql)));
    return std::get<0>(rows.at(0));
}
} // namespace

AttachmentService::AttachmentService(
    std::unique_ptr<AttachmentDataSqlite> storage,
    const Configuration& settings)
    : data(std::move(storage)), config(settings),
      root(settings.attachment_dir.empty()
               ? std::filesystem::path(settings.data_dir) / "attachments"
               : std::filesystem::path(settings.attachment_dir))
{
}

E<void> AttachmentService::initialize()
try
{
    std::lock_guard lock(mutation_mutex);
    DO_OR_RETURN(validateImageSettings(config));
    DO_OR_RETURN(initializeImageConverter());
    DO_OR_RETURN(data->initializeSchema());
    std::error_code error;
    std::filesystem::create_directories(root / ".staging", error);
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    for(const auto& entry :
        std::filesystem::directory_iterator(root / ".staging", error))
    {
        if(!entry.is_regular_file() || entry.is_symlink())
        {
            spdlog::warn("Unknown staging entry {}", entry.path().string());
            continue;
        }
        std::filesystem::remove(entry.path(), error);
        if(error)
        {
            return std::unexpected(runtimeError(error.message()));
        }
    }
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    DO_OR_RETURN(reclaim());
    ASSIGN_OR_RETURN(auto files,
                     (data->db->eval<std::string, int64_t>(
                         "SELECT hash,byte_size FROM AttachmentFiles")));
    std::set<std::string> known;
    for(const auto& [hash, size] : files)
    {
        known.insert(hash);
        ASSIGN_OR_RETURN(auto path, attachmentPath(root, hash));
        ASSIGN_OR_RETURN(auto actual, inspectAttachment(path));
        if(actual.hash != hash || actual.byte_size != size)
        {
            return std::unexpected(
                runtimeError("Missing or corrupt attachment: " + hash));
        }
    }
    for(const auto& shard : std::filesystem::directory_iterator(root, error))
    {
        if(shard.path().filename() == ".staging")
        {
            continue;
        }
        auto prefix = shard.path().filename().string();
        if(prefix.size() != 2 ||
           !attachmentHash(prefix + std::string(30, '0')) ||
           !shard.is_directory() || shard.is_symlink())
        {
            spdlog::warn("Unknown attachment entry {}", shard.path().string());
            continue;
        }
        for(const auto& entry :
            std::filesystem::directory_iterator(shard.path(), error))
        {
            auto hash = entry.path().filename().string();
            if(!attachmentHash(hash) || hash.substr(0, 2) != prefix ||
               !entry.is_regular_file() || entry.is_symlink())
            {
                spdlog::warn("Unknown attachment entry {}",
                             entry.path().string());
                continue;
            }
            if(!known.contains(hash))
            {
                std::filesystem::remove(entry.path(), error);
                if(error)
                {
                    return std::unexpected(runtimeError(error.message()));
                }
            }
        }
    }
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    return {};
}

catch(const std::filesystem::filesystem_error& error)
{
    return std::unexpected(runtimeError(error.what()));
}

E<AttachmentWriter> AttachmentService::beginUpload(int64_t user,
                                                   std::string_view filename)
{
    ASSIGN_OR_RETURN(auto name, attachmentName(filename));
    {
        std::lock_guard lock(mutation_mutex);
        DO_OR_RETURN(data->owner(user));
    }
    ASSIGN_OR_RETURN(auto staging, AttachmentStaging::create(root));
    return AttachmentWriter(*this, user, std::move(name), std::move(staging));
}
E<void> AttachmentWriter::append(std::string_view bytes)
{
    if(!staging)
    {
        return std::unexpected(runtimeError("Upload closed"));
    }
    if(service->config.attachment_upload_limit &&
       bytes.size() > *service->config.attachment_upload_limit -
                          static_cast<uint64_t>(staging->size()))
    {
        staging.reset();
        return std::unexpected(httpError(413, "Upload exceeds byte limit"));
    }
    auto result = staging->append(bytes);
    if(!result)
    {
        staging.reset();
    }
    return result;
}
E<AttachmentUpload> AttachmentWriter::finish()
{
    if(!staging)
    {
        return std::unexpected(runtimeError("Upload closed"));
    }
    auto result = service->finish(*this);
    staging.reset();
    return result;
}
E<AttachmentFile> AttachmentService::adopt(const AttachmentStaging& staging,
                                           AttachmentFile file)
{
    ASSIGN_OR_RETURN(auto sql, data->db->statementFromStr(
                                   "SELECT byte_size,mime_type,created FROM "
                                   "AttachmentFiles WHERE hash=?"));
    DO_OR_RETURN(sql.bind(file.hash));
    ASSIGN_OR_RETURN(auto rows, (data->db->eval<int64_t, std::string, int64_t>(
                                    std::move(sql))));
    ASSIGN_OR_RETURN(auto path, attachmentPath(root, file.hash));
    if(!rows.empty())
    {
        std::error_code error;
        if(!std::filesystem::is_regular_file(path, error))
        {
            return std::unexpected(
                runtimeError("Referenced attachment is missing"));
        }
        auto& [size, mime, created] = rows[0];
        file.byte_size = size;
        file.mime_type = mime;
        file.created = created;
    }
    else
    {
        DO_OR_RETURN(publishAttachment(root, staging.path(), file));
        // A pre-existing orphan may have different bytes with the same
        // identity.
        ASSIGN_OR_RETURN(auto actual, inspectAttachment(path));
        file.byte_size = actual.byte_size;
        file.mime_type = actual.mime_type;
        DO_OR_RETURN(
            execute(*data->db, "INSERT INTO AttachmentFiles VALUES(?,?,?,?)",
                    file.hash, file.byte_size, file.mime_type, file.created));
    }
    DO_OR_RETURN(execute(
        *data->db, "DELETE FROM AttachmentCleanup WHERE hash=?", file.hash));
    return file;
}
E<AttachmentUpload> AttachmentService::finish(AttachmentWriter& writer)
{
    ASSIGN_OR_RETURN(auto file, writer.staging->finish());
    std::lock_guard lock(mutation_mutex);
    DO_OR_RETURN(data->owner(writer.user_id));
    AttachmentTransaction transaction(*data->db);
    DO_OR_RETURN(transaction.begin());
    ASSIGN_OR_RETURN(file, adopt(*writer.staging, file));
    ASSIGN_OR_RETURN(auto existing, data->list(writer.user_id));
    for(auto& record : existing)
    {
        if(record.file.hash == file.hash &&
           record.original_name == writer.filename)
        {
            DO_OR_RETURN(transaction.commit());
            ASSIGN_OR_RETURN(auto url,
                             attachmentURL(file.hash, writer.filename));
            return AttachmentUpload{record, url, true};
        }
    }
    DO_OR_RETURN(execute(
        *data->db,
        "INSERT INTO Attachments(user_id,file_hash,original_name,added) "
        "VALUES(?,?,?,?)",
        writer.user_id, file.hash, writer.filename,
        static_cast<int64_t>(time(nullptr))));
    ASSIGN_OR_RETURN(auto record,
                     get(writer.user_id, data->db->lastInsertRowID()));
    DO_OR_RETURN(transaction.commit());
    ASSIGN_OR_RETURN(auto url, attachmentURL(file.hash, writer.filename));
    return AttachmentUpload{record, url, false};
}
E<std::vector<Attachment>> AttachmentService::listAttachments(int64_t user)
{
    std::lock_guard lock(mutation_mutex);
    return data->list(user);
}
E<Attachment> AttachmentService::get(int64_t user, int64_t id)
{
    ASSIGN_OR_RETURN(auto records, data->list(user));
    for(auto& record : records)
    {
        if(record.id == id)
        {
            return record;
        }
    }
    return std::unexpected(httpError(404, "Attachment not found"));
}
E<Attachment> AttachmentService::getAttachment(int64_t user, int64_t id)
{
    std::lock_guard lock(mutation_mutex);
    return get(user, id);
}
E<void> AttachmentService::removeOutgoing(const std::string& hash)
{
    DO_OR_RETURN(execute(*data->db, R"(
INSERT OR IGNORE INTO AttachmentCleanup SELECT output_hash
 FROM AttachmentVariants WHERE source_hash=?)",
                         hash));
    return execute(*data->db,
                   "DELETE FROM AttachmentVariants WHERE source_hash=?", hash);
}
E<void> AttachmentService::deleteAttachment(int64_t user, int64_t id)
{
    std::lock_guard lock(mutation_mutex);
    ASSIGN_OR_RETURN(auto record, get(user, id));
    AttachmentTransaction transaction(*data->db);
    DO_OR_RETURN(transaction.begin());
    DO_OR_RETURN(execute(*data->db,
                         "DELETE FROM Attachments WHERE id=? AND user_id=?", id,
                         user));
    ASSIGN_OR_RETURN(auto sql,
                     data->db->statementFromStr(
                         "SELECT count(*) FROM Attachments WHERE file_hash=?"));
    DO_OR_RETURN(sql.bind(record.file.hash));
    ASSIGN_OR_RETURN(auto owners, data->db->eval<int64_t>(std::move(sql)));
    if(std::get<0>(owners[0]) == 0)
    {
        DO_OR_RETURN(removeOutgoing(record.file.hash));
        DO_OR_RETURN(execute(
            *data->db, "INSERT OR IGNORE INTO AttachmentCleanup VALUES(?)",
            record.file.hash));
    }
    DO_OR_RETURN(transaction.commit());
    auto result = reclaim();
    if(!result)
    {
        spdlog::error("Attachment cleanup: {}", errorMsg(result.error()));
    }
    return {};
}
E<void> AttachmentService::cleanup()
{
    std::lock_guard lock(mutation_mutex);
    return reclaim();
}
E<void> AttachmentService::reclaim()
{
    std::set<std::string> visited;
    while(true)
    {
        ASSIGN_OR_RETURN(
            auto pending,
            data->db->eval<std::string>("SELECT hash FROM AttachmentCleanup"));
        bool progress = false;
        for(const auto& [hash] : pending)
        {
            if(!visited.insert(hash).second)
            {
                continue;
            }
            progress = true;
            ASSIGN_OR_RETURN(auto count, references(*data->db, hash));
            if(count != 0)
            {
                DO_OR_RETURN(execute(
                    *data->db, "DELETE FROM AttachmentCleanup WHERE hash=?",
                    hash));
                continue;
            }
            {
                AttachmentTransaction transaction(*data->db);
                DO_OR_RETURN(transaction.begin());
                DO_OR_RETURN(removeOutgoing(hash));
                DO_OR_RETURN(transaction.commit());
            }
            ASSIGN_OR_RETURN(auto path, attachmentPath(root, hash));
            std::error_code error;
            std::filesystem::remove(path, error);
            if(error)
            {
                spdlog::error("Attachment unlink {}: {}", hash,
                              error.message());
                continue;
            }
            int directory =
                open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
            if(directory >= 0)
            {
                int status = fsync(directory);
                close(directory);
                if(status != 0)
                {
                    spdlog::error("Attachment deletion sync failed: {}", hash);
                    continue;
                }
            }
            AttachmentTransaction transaction(*data->db);
            DO_OR_RETURN(transaction.begin());
            DO_OR_RETURN(execute(
                *data->db, "DELETE FROM AttachmentCleanup WHERE hash=?", hash));
            DO_OR_RETURN(execute(
                *data->db, "DELETE FROM AttachmentFiles WHERE hash=?", hash));
            DO_OR_RETURN(transaction.commit());
        }
        if(!progress)
        {
            break;
        }
    }
    return {};
}

E<std::optional<ImageVariant>>
AttachmentService::cached(const std::string& source, const std::string& recipe,
                          const std::string& name)
{
    ASSIGN_OR_RETURN(auto sql, data->db->statementFromStr(R"(
SELECT f.hash,f.mime_type,f.byte_size,f.created,v.width,v.height,v.format
 FROM AttachmentVariants v JOIN AttachmentFiles f ON f.hash=v.output_hash
 WHERE v.source_hash=? AND v.recipe=?)"));
    DO_OR_RETURN(sql.bind(source, recipe));
    ASSIGN_OR_RETURN(auto rows,
                     (data->db->eval<std::string, std::string, int64_t, int64_t,
                                     int, int, std::string>(std::move(sql))));
    if(rows.empty())
    {
        return std::optional<ImageVariant>{};
    }
    auto& [hash, mime, size, created, width, height, format] = rows[0];
    ASSIGN_OR_RETURN(
        auto url,
        attachmentURL(hash, std::filesystem::path(name).stem().string() + "." +
                                format));
    return std::optional<ImageVariant>{
        {{hash, mime, size, created}, format, url, width, height}};
}
E<ImageVariant>
AttachmentService::ensureImageVariant(int64_t user, int64_t id,
                                      const ImageVariantOptions& options)
{
    ASSIGN_OR_RETURN(auto recipe, imageRecipe(options, config));
    ASSIGN_OR_RETURN(auto record, getAttachment(user, id));
    std::string key = record.file.hash + ";" + recipe;
    std::shared_ptr<std::mutex> recipe_mutex;
    {
        std::lock_guard lock(generation_mutex);
        for(auto it = generation_locks.begin(); it != generation_locks.end();)
        {
            if(it->second.expired())
            {
                it = generation_locks.erase(it);
            }
            else
            {
                ++it;
            }
        }
        recipe_mutex = generation_locks[key].lock();
        if(!recipe_mutex)
        {
            recipe_mutex = std::make_shared<std::mutex>();
            generation_locks[key] = recipe_mutex;
        }
    }
    std::lock_guard recipe_lock(*recipe_mutex);
    int descriptor;
    {
        std::lock_guard lock(mutation_mutex);
        DO_OR_RETURN(get(user, id));
        ASSIGN_OR_RETURN(auto existing, cached(record.file.hash, recipe,
                                               record.original_name));
        if(existing)
        {
            return *existing;
        }
        ASSIGN_OR_RETURN(auto path, attachmentPath(root, record.file.hash));
        descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if(descriptor < 0)
    {
        return std::unexpected(runtimeError("Image source missing"));
    }
    struct Input
    {
        int fd;
        ~Input()
        {
            close(fd);
        }
    } input{descriptor};
    {
        std::unique_lock lock(generation_mutex);
        generation_condition.wait(
            lock,
            [this]
            {
                return active_conversions < config.attachment_image_concurrency;
            });
        ++active_conversions;
    }
    struct Slot
    {
        AttachmentService& service;
        ~Slot()
        {
            std::lock_guard lock(service.generation_mutex);
            --service.active_conversions;
            service.generation_condition.notify_one();
        }
    } slot{*this};
    ASSIGN_OR_RETURN(auto staging, AttachmentStaging::create(root));
    ASSIGN_OR_RETURN(auto dimensions,
                     convertImage(descriptor, record.file.mime_type, options,
                                  config, *staging));
    ASSIGN_OR_RETURN(auto file, staging->finish());
    std::lock_guard lock(mutation_mutex);
    ASSIGN_OR_RETURN(auto current, get(user, id));
    if(current.file.hash != record.file.hash)
    {
        return std::unexpected(httpError(404, "Attachment changed"));
    }
    AttachmentTransaction transaction(*data->db);
    DO_OR_RETURN(transaction.begin());
    ASSIGN_OR_RETURN(file, adopt(*staging, file));
    DO_OR_RETURN(execute(*data->db,
                         "INSERT INTO AttachmentVariants VALUES(?,?,?,?,?,?,?)",
                         record.file.hash, recipe, file.hash, dimensions.first,
                         dimensions.second, options.format,
                         static_cast<int64_t>(time(nullptr))));
    DO_OR_RETURN(transaction.commit());
    ASSIGN_OR_RETURN(auto result,
                     cached(record.file.hash, recipe, record.original_name));
    return *result;
}
