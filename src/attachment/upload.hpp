#pragma once
#include "attachment/attachment_data.hpp"
#include "attachment/attachment_files.hpp"
#include <functional>

class AttachmentService;
class UploadExecution;
class UploadProcessing;

// The same publication value is returned to every upload consumer.
using UploadResult = AttachmentUpload;

// Initial identity and optional synchronous chunk source; empty chunk is EOF.
struct UploadInput
{
    // Existing owner and client-supplied basename, validated by the service.
    int64_t user_id;
    std::string filename;
    // Reads bounded chunks within the request lifetime; errors cancel the task.
    std::function<E<std::string>()> source = {};
};

// Borrowed metadata access; transaction control remains with the service.
class UploadDatabase
{
public:
    // Prepares consumer SQL without granting transaction-control operations.
    E<SQLiteStatement> statementFromStr(const char* sql);
    // Evaluates a prepared parameterized consumer query.
    template <class... Types>
    E<std::vector<std::tuple<Types...>>> eval(SQLiteStatement sql) const
    {
        return db.eval<Types...>(std::move(sql));
    }
    // Evaluates a consumer query using this same connection.
    template <class... Types>
    E<std::vector<std::tuple<Types...>>> eval(const char* sql)
    {
        ASSIGN_OR_RETURN(auto statement, statementFromStr(sql));
        return db.eval<Types...>(std::move(statement));
    }
    // Executes a previously prepared consumer statement.
    E<void> execute(SQLiteStatement sql) const
    {
        return db.execute(std::move(sql));
    }
    // Executes consumer SQL without opening or committing a transaction.
    E<void> execute(const char* sql)
    {
        ASSIGN_OR_RETURN(auto statement, statementFromStr(sql));
        return db.execute(std::move(statement));
    }
    // Identifies the last consumer row inserted on this connection.
    int64_t lastInsertRowID() const
    {
        return db.lastInsertRowID();
    }
    UploadDatabase(const UploadDatabase&) = delete;
    UploadDatabase& operator=(const UploadDatabase&) = delete;

private:
    friend class AttachmentService;
    explicit UploadDatabase(SQLite& connection) : db(connection) {}
    SQLite& db;
};

// Validated immutable identity available before receiving any bytes.
struct UploadStart
{
    // Existing owner and sanitized filename.
    const UploadInput& input;
};

// Attachment identity and borrowed metadata access inside publication only.
struct UploadPublication
{
    // Resolved attachment, including whether its named reference already
    // existed.
    const UploadResult& attachment;
    // Consumer writes share the service's active transaction.
    UploadDatabase& database;
};

// Consumer-specific task; only the attachment service drives these hooks.
class Upload
{
public:
    // Allows destruction after transfer through the base interface.
    virtual ~Upload() = default;
    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;
    // Transfers initial input once; repeated transfer returns an error.
    E<UploadInput> takeInput();
    // Validates consumer state before reception starts.
    virtual E<void> beginUpload(UploadStart& context) = 0;
    // Adds consumer processing after the source is fully staged.
    virtual E<void> process(UploadProcessing& context) = 0;
    // Writes consumer associations in the service-owned transaction.
    virtual E<void> endUpload(UploadPublication& context) = 0;

protected:
    // Stores immutable request input until the service starts execution.
    explicit Upload(UploadInput input) : input(std::move(input)) {}

private:
    std::optional<UploadInput> input;
};

// Default task for consumers needing no processing or additional association.
class OrdinaryUpload : public Upload
{
public:
    // Supplies the common identity and optional bounded chunk source.
    explicit OrdinaryUpload(UploadInput input) : Upload(std::move(input)) {}
    // Ordinary uploads need no extra validation after common checks.
    E<void> beginUpload(UploadStart&) override
    {
        return {};
    }
    // Ordinary uploads preserve the original without derivatives.
    E<void> process(UploadProcessing&) override
    {
        return {};
    }
    // Ordinary uploads have no consumer table association.
    E<void> endUpload(UploadPublication&) override
    {
        return {};
    }
};

// Service-owned streaming handle and per-upload resources, never consumer
// policy.
class UploadExecution
{
public:
    // Transfers the lifecycle without copying file or digest state.
    UploadExecution(UploadExecution&& other) noexcept;
    UploadExecution& operator=(UploadExecution&&) = delete;
    UploadExecution(const UploadExecution&) = delete;
    // Cancels incomplete execution through the borrowed service.
    ~UploadExecution();
    // Delegates bounded reception to the service.
    E<void> append(std::string_view bytes);
    // Delegates preparation and atomic publication to the service, once.
    E<UploadResult> finish();

private:
    friend class AttachmentService;
    friend class UploadProcessing;
    struct Variant
    {
        ImageVariantOptions options;
        std::string recipe;
        std::unique_ptr<AttachmentStaging> staging;
        AttachmentFile file;
        std::pair<int, int> dimensions;
    };
    UploadExecution(AttachmentService& owner, std::unique_ptr<Upload> task,
                    UploadInput input);
    AttachmentService* service;
    std::unique_ptr<Upload> task;
    UploadInput input;
    std::unique_ptr<AttachmentStaging> staging;
    AttachmentFile file;
    std::vector<Variant> variants;
    std::optional<Attachment> existing;
    std::filesystem::path manifest;
    enum class Phase
    {
        STARTING,
        RECEIVING,
        PREPARING,
        PUBLISHING,
        CLOSED
    };
    Phase phase = Phase::STARTING;
};

// Generic staged-source operations available during the consumer process hook.
class UploadProcessing
{
public:
    // Immutable source metadata after reception and content probing.
    const AttachmentFile& source() const;
    // Advisory named attachment identity; publication revalidates it.
    const std::optional<Attachment>& existingAttachment() const;
    // Stages a bounded derivative without publishing metadata or files.
    E<void> prepareVariant(const ImageVariantOptions& options);
    // Runs consumer advisory queries under the common metadata mutex.
    E<void> readMetadata(const std::function<E<void>(UploadDatabase&)>& read);

private:
    friend class AttachmentService;
    explicit UploadProcessing(UploadExecution& state) : state(state) {}
    UploadExecution& state;
};

// Compatibility name for the ordinary streaming handle; same common executor.
using AttachmentWriter = UploadExecution;
