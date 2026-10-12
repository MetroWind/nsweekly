#include "attachment/attachment_service.hpp"
#include "attachment/image_variant.hpp"
#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <fstream>
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
std::string sqlCommand(std::string_view sql)
{
    while(!sql.empty())
    {
        auto begin = sql.find_first_not_of(" \t\r\n");
        if(begin == sql.npos)
        {
            return {};
        }
        sql.remove_prefix(begin);
        if(sql.starts_with("--"))
        {
            auto end = sql.find('\n');
            if(end == sql.npos)
            {
                return {};
            }
            sql.remove_prefix(end + 1);
        }
        else if(sql.starts_with("/*"))
        {
            auto end = sql.find("*/", 2);
            if(end == sql.npos)
            {
                return {};
            }
            sql.remove_prefix(end + 2);
        }
        else
        {
            break;
        }
    }
    std::string command;
    for(unsigned char c : sql)
    {
        if(!std::isalpha(c))
        {
            break;
        }
        command += static_cast<char>(std::toupper(c));
    }
    return command;
}

} // namespace

E<SQLiteStatement> UploadDatabase::statementFromStr(const char* sql)
{
    // This is an internal programming contract, not an untrusted SQL sandbox.
    auto command = sqlCommand(sql);
    if(command == "BEGIN" || command == "COMMIT" || command == "END" ||
       command == "ROLLBACK" || command == "SAVEPOINT" || command == "RELEASE")
    {
        return std::unexpected(
            runtimeError("Consumer transaction control forbidden"));
    }
    return db.statementFromStr(sql);
}

E<UploadInput> Upload::takeInput()
{
    if(!input)
    {
        return std::unexpected(
            runtimeError("Upload input already transferred"));
    }
    auto result = std::move(*input);
    input.reset();
    return result;
}

UploadExecution::UploadExecution(AttachmentService& owner,
                                 std::unique_ptr<Upload> upload_task,
                                 UploadInput upload_input)
    : service(&owner), task(std::move(upload_task)),
      input(std::move(upload_input))
{
}
UploadExecution::UploadExecution(UploadExecution&& other) noexcept
    : service(std::exchange(other.service, nullptr)),
      task(std::move(other.task)), input(std::move(other.input)),
      staging(std::move(other.staging)), file(std::move(other.file)),
      variants(std::move(other.variants)), existing(std::move(other.existing)),
      manifest(std::move(other.manifest)), phase(other.phase)
{
}
UploadExecution::~UploadExecution()
{
    if(service)
    {
        service->cancel(*this);
    }
}
E<void> UploadExecution::append(std::string_view bytes)
{
    if(!service)
    {
        return std::unexpected(runtimeError("Upload closed"));
    }
    return service->receive(*this, bytes);
}
E<UploadResult> UploadExecution::finish()
{
    if(!service)
    {
        return std::unexpected(runtimeError("Upload closed"));
    }
    return service->complete(*this);
}
const AttachmentFile& UploadProcessing::source() const
{
    return state.file;
}
const std::optional<Attachment>& UploadProcessing::existingAttachment() const
{
    return state.existing;
}
E<void> UploadProcessing::prepareVariant(const ImageVariantOptions& options)
{
    return state.service->prepareVariant(state, options);
}
E<void> UploadProcessing::readMetadata(
    const std::function<E<void>(UploadDatabase&)>& read)
{
    return state.service->readMetadata(read);
}

E<void> AttachmentService::readMetadata(
    const std::function<E<void>(UploadDatabase&)>& read)
{
    std::lock_guard lock(mutation_mutex);
    UploadDatabase view(*data->db);
    return read(view);
}
E<void> AttachmentService::writeMetadata(
    const std::function<E<void>(UploadDatabase&)>& write)
{
    std::lock_guard lock(mutation_mutex);
    AttachmentTransaction transaction(*data->db);
    DO_OR_RETURN(transaction.begin());
    UploadDatabase view(*data->db);
    DO_OR_RETURN(write(view));
    return transaction.commit();
}

E<std::unique_ptr<UploadExecution>>
AttachmentService::startUpload(std::unique_ptr<Upload> task)
{
    if(!task)
    {
        return std::unexpected(runtimeError("Missing upload task"));
    }
    ASSIGN_OR_RETURN(auto input, task->takeInput());
    auto state = std::unique_ptr<UploadExecution>(
        new UploadExecution(*this, std::move(task), std::move(input)));
    DO_OR_RETURN(startExecution(*state));
    return state;
}
E<UploadResult> AttachmentService::doUpload(std::unique_ptr<Upload> task)
{
    ASSIGN_OR_RETURN(auto state, startUpload(std::move(task)));
    if(!state->input.source)
    {
        return std::unexpected(runtimeError("Missing upload byte source"));
    }
    while(true)
    {
        ASSIGN_OR_RETURN(auto bytes, state->input.source());
        if(bytes.empty())
        {
            break;
        }
        DO_OR_RETURN(state->append(bytes));
    }
    return state->finish();
}
E<AttachmentWriter> AttachmentService::beginUpload(int64_t user,
                                                   std::string_view filename)
{
    ASSIGN_OR_RETURN(auto state,
                     startUpload(std::make_unique<OrdinaryUpload>(
                         UploadInput{user, std::string(filename)})));
    return std::move(*state);
}
E<void> AttachmentService::startExecution(UploadExecution& state)
{
    ASSIGN_OR_RETURN(state.input.filename,
                     attachmentName(state.input.filename));
    {
        std::lock_guard lock(mutation_mutex);
        DO_OR_RETURN(data->owner(state.input.user_id));
        ASSIGN_OR_RETURN(state.staging, AttachmentStaging::create(root));
        state.manifest = state.staging->path().string() + ".manifest";
        active_uploads.insert(state.manifest);
    }
    UploadStart context{state.input};
    DO_OR_RETURN(state.task->beginUpload(context));
    state.phase = UploadExecution::Phase::RECEIVING;
    return {};
}
E<void> AttachmentService::receive(UploadExecution& state,
                                   std::string_view bytes)
{
    if(state.phase != UploadExecution::Phase::RECEIVING)
    {
        return std::unexpected(runtimeError("Upload is not receiving"));
    }
    if(config.attachment_upload_limit &&
       bytes.size() > *config.attachment_upload_limit -
                          static_cast<uint64_t>(state.staging->size()))
    {
        cancel(state);
        return std::unexpected(httpError(413, "Upload exceeds byte limit"));
    }
    auto result = state.staging->append(bytes);
    if(!result)
    {
        cancel(state);
    }
    return result;
}
E<void> AttachmentService::prepare(UploadExecution& state)
{
    state.phase = UploadExecution::Phase::PREPARING;
    ASSIGN_OR_RETURN(state.file, state.staging->finish());
    {
        std::lock_guard lock(mutation_mutex);
        ASSIGN_OR_RETURN(auto records, data->list(state.input.user_id));
        for(auto& record : records)
        {
            if(record.file.hash == state.file.hash &&
               record.original_name == state.input.filename)
            {
                state.existing = std::move(record);
                break;
            }
        }
    }
    UploadProcessing context(state);
    return state.task->process(context);
}
E<void> AttachmentService::prepareVariant(UploadExecution& state,
                                          const ImageVariantOptions& options)
{
    if(state.phase != UploadExecution::Phase::PREPARING)
    {
        return std::unexpected(runtimeError("Upload is not processing"));
    }
    ASSIGN_OR_RETURN(auto recipe, imageRecipe(options, config));
    for(const auto& variant : state.variants)
    {
        if(variant.recipe == recipe)
        {
            return {};
        }
    }
    {
        std::lock_guard lock(mutation_mutex);
        ASSIGN_OR_RETURN(auto hit,
                         cached(state.file.hash, recipe, state.input.filename));
        if(hit)
        {
            state.variants.push_back({options,
                                      recipe,
                                      nullptr,
                                      hit->file,
                                      {hit->width, hit->height}});
            return {};
        }
    }
    int descriptor =
        open(state.staging->path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor < 0)
    {
        return std::unexpected(runtimeError("Staged image source missing"));
    }
    struct Input
    {
        int fd;
        ~Input()
        {
            close(fd);
        }
    } input{descriptor};
    ASSIGN_OR_RETURN(auto staging, AttachmentStaging::create(root));
    ASSIGN_OR_RETURN(
        auto dimensions,
        convertVariant(descriptor, state.file.mime_type, options, *staging));
    ASSIGN_OR_RETURN(auto file, staging->finish());
    state.variants.push_back({options, std::move(recipe), std::move(staging),
                              std::move(file), dimensions});
    return {};
}

E<UploadResult> AttachmentService::publish(UploadExecution& state)
{
    state.phase = UploadExecution::Phase::PUBLISHING;
    std::lock_guard lock(mutation_mutex);
    DO_OR_RETURN(data->owner(state.input.user_id));
    AttachmentTransaction transaction(*data->db);
    DO_OR_RETURN(transaction.begin());
    std::optional<Attachment> existing;
    ASSIGN_OR_RETURN(auto records, data->list(state.input.user_id));
    for(auto& record : records)
    {
        if(record.file.hash == state.file.hash &&
           record.original_name == state.input.filename)
        {
            existing = std::move(record);
            break;
        }
    }
    if(state.existing && (!existing || existing->id != state.existing->id))
    {
        return std::unexpected(
            httpError(409, "Attachment changed; retry upload"));
    }
    DO_OR_RETURN(writeManifest(state));
    ASSIGN_OR_RETURN(auto file, adopt(*state.staging, state.file));
    Attachment record;
    if(existing)
    {
        record = *existing;
    }
    else
    {
        DO_OR_RETURN(execute(
            *data->db,
            "INSERT INTO Attachments(user_id,file_hash,original_name,added) "
            "VALUES(?,?,?,?)",
            state.input.user_id, file.hash, state.input.filename,
            static_cast<int64_t>(time(nullptr))));
        ASSIGN_OR_RETURN(record,
                         get(state.input.user_id, data->db->lastInsertRowID()));
    }
    for(auto& variant : state.variants)
    {
        ASSIGN_OR_RETURN(
            auto hit, cached(file.hash, variant.recipe, state.input.filename));
        if(hit)
        {
            continue;
        }
        if(!variant.staging)
        {
            return std::unexpected(
                httpError(409, "Cached variant changed; retry"));
        }
        ASSIGN_OR_RETURN(auto output, adopt(*variant.staging, variant.file));
        DO_OR_RETURN(execute(
            *data->db, "INSERT INTO AttachmentVariants VALUES(?,?,?,?,?,?,?)",
            file.hash, variant.recipe, output.hash, variant.dimensions.first,
            variant.dimensions.second, variant.options.format,
            static_cast<int64_t>(time(nullptr))));
    }
    ASSIGN_OR_RETURN(auto url, attachmentURL(file.hash, state.input.filename));
    UploadResult result{record, std::move(url), existing.has_value()};
    UploadDatabase database(*data->db);
    UploadPublication context{result, database};
    DO_OR_RETURN(state.task->endUpload(context));
    DO_OR_RETURN(transaction.commit());
    return result;
}
E<UploadResult> AttachmentService::complete(UploadExecution& state)
{
    if(state.phase != UploadExecution::Phase::RECEIVING)
    {
        return std::unexpected(runtimeError("Upload is not receiving"));
    }
    auto prepared = prepare(state);
    if(!prepared)
    {
        cancel(state);
        return std::unexpected(prepared.error());
    }
    auto result = publish(state);
    // publish's transaction has committed or rolled back before cleanup runs.
    cancel(state);
    return result;
}
void AttachmentService::cancel(UploadExecution& state)
{
    if(state.phase == UploadExecution::Phase::CLOSED)
    {
        return;
    }
    state.phase = UploadExecution::Phase::CLOSED;
    state.staging.reset();
    state.variants.clear();
    {
        std::lock_guard lock(mutation_mutex);
        active_uploads.erase(state.manifest);
        if(!state.manifest.empty())
        {
            auto recovered = recoverManifest(state.manifest);
            if(!recovered)
            {
                spdlog::error("Upload cleanup: {}",
                              errorMsg(recovered.error()));
            }
        }
    }
    state.task.reset();
    state.service = nullptr;
}

namespace
{
E<void> syncDirectory(const std::filesystem::path& path)
{
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0)
    {
        return std::unexpected(runtimeError("Cannot open directory for sync"));
    }
    int status = fsync(fd);
    close(fd);
    if(status != 0)
    {
        return std::unexpected(
            runtimeError("Cannot sync attachment directory"));
    }
    return {};
}
} // namespace

E<void> AttachmentService::writeManifest(UploadExecution& state)
{
    std::string contents = state.file.hash + "\n";
    for(const auto& variant : state.variants)
    {
        contents += variant.file.hash + "\n";
    }
    ASSIGN_OR_RETURN(auto temporary, AttachmentStaging::create(root));
    DO_OR_RETURN(temporary->append(contents));
    DO_OR_RETURN(temporary->finish());
    std::error_code error;
    std::filesystem::rename(temporary->path(), state.manifest, error);
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    return syncDirectory(root / ".staging");
}
E<void>
AttachmentService::recoverManifest(const std::filesystem::path& manifest)
{
    std::error_code error;
    if(!std::filesystem::exists(manifest, error))
    {
        if(error)
        {
            return std::unexpected(runtimeError(error.message()));
        }
        return {};
    }
    std::ifstream input(manifest);
    if(!input)
    {
        return std::unexpected(runtimeError("Cannot read upload manifest"));
    }
    std::string hash;
    while(std::getline(input, hash))
    {
        if(!attachmentHash(hash))
        {
            return std::unexpected(
                runtimeError("Invalid upload manifest hash"));
        }
        // Rows surviving rollback or a successful commit own their bytes.
        ASSIGN_OR_RETURN(
            auto sql, data->db->statementFromStr(
                          "SELECT count(*) FROM AttachmentFiles WHERE hash=?"));
        DO_OR_RETURN(sql.bind(hash));
        ASSIGN_OR_RETURN(auto rows, data->db->eval<int64_t>(std::move(sql)));
        if(std::get<0>(rows[0]) != 0)
        {
            continue;
        }
        ASSIGN_OR_RETURN(auto path, attachmentPath(root, hash));
        std::filesystem::remove(path, error);
        if(error)
        {
            return std::unexpected(runtimeError(error.message()));
        }
        // Retry a previous failed directory sync even if unlink already ran.
        bool shard_exists = std::filesystem::exists(path.parent_path(), error);
        if(error)
        {
            return std::unexpected(runtimeError(error.message()));
        }
        if(shard_exists)
        {
            DO_OR_RETURN(syncDirectory(path.parent_path()));
        }
    }
    if(!input.eof())
    {
        return std::unexpected(
            runtimeError("Cannot finish reading upload manifest"));
    }
    std::filesystem::remove(manifest, error);
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    return syncDirectory(root / ".staging");
}
E<void> AttachmentService::recoverUploads()
{
    std::error_code error;
    for(const auto& entry :
        std::filesystem::directory_iterator(root / ".staging", error))
    {
        if(entry.path().extension() == ".manifest" && entry.is_regular_file() &&
           !entry.is_symlink() && !active_uploads.contains(entry.path()))
        {
            DO_OR_RETURN(recoverManifest(entry.path()));
        }
    }
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    return {};
}
