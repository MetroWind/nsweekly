#pragma once
#include "attachment/attachment_data.hpp"
#include "attachment/attachment_files.hpp"
#include "config.hpp"
#include "attachment/upload.hpp"
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

// Coordinates immutable public files, ownership, variants and reclamation.
class AttachmentService
{
public:
    // Takes a dedicated initialized SQLite connection and immutable settings.
    AttachmentService(std::unique_ptr<AttachmentDataSqlite> data,
                      const Configuration& settings);
    // Creates storage, removes interrupted work, and validates persisted bytes.
    E<void> initialize();
    // Starts an ordinary task through the common executor for existing callers.
    E<AttachmentWriter> beginUpload(int64_t user, std::string_view filename);
    // Takes exclusive ownership and executes a task's bounded source
    // synchronously.
    E<UploadResult> doUpload(std::unique_ptr<Upload> upload_task);
    // Takes exclusive ownership for request-local callback streaming.
    E<std::unique_ptr<UploadExecution>>
    startUpload(std::unique_ptr<Upload> upload_task);
    // Borrows serialized metadata access for consumer-owned read helpers.
    E<void> readMetadata(const std::function<E<void>(UploadDatabase&)>& read);
    // Runs consumer-owned schema or mutations inside a common transaction.
    E<void> writeMetadata(const std::function<E<void>(UploadDatabase&)>& write);
    // Lists only this owner's named references.
    E<std::vector<Attachment>> listAttachments(int64_t user);
    // Returns 404 for both absent and foreign records.
    E<Attachment> getAttachment(int64_t user, int64_t id);
    // Commits logical removal and retries reference-safe physical cleanup.
    E<void> deleteAttachment(int64_t user, int64_t id);
    // Converts on demand and reuses persisted versioned recipes.
    E<ImageVariant> ensureImageVariant(int64_t user, int64_t id,
                                       const ImageVariantOptions& options);
    // Retries pending physical deletion without resurrecting records.
    E<void> cleanup();
    // Supplies the validated root to the public file-serving adapter.
    const std::filesystem::path& directory() const
    {
        return root;
    }

private:
    friend class UploadExecution;
    friend class UploadProcessing;
    E<void> startExecution(UploadExecution& state);
    E<void> receive(UploadExecution& state, std::string_view bytes);
    E<void> prepare(UploadExecution& state);
    E<void> prepareVariant(UploadExecution& state,
                           const ImageVariantOptions& options);
    E<std::pair<int, int>> convertVariant(int descriptor,
                                          const std::string& mime,
                                          const ImageVariantOptions& options,
                                          AttachmentStaging& staging);
    E<UploadResult> publish(UploadExecution& state);
    E<UploadResult> complete(UploadExecution& state);
    void cancel(UploadExecution& state);
    E<void> recoverUploads();
    E<void> recoverManifest(const std::filesystem::path& manifest);
    E<void> writeManifest(UploadExecution& state);
    std::set<std::filesystem::path> active_uploads;
    E<Attachment> get(int64_t user, int64_t id);
    E<AttachmentFile> adopt(const AttachmentStaging& staging,
                            AttachmentFile file);
    E<void> reclaim();
    E<void> removeOutgoing(const std::string& hash);
    E<std::optional<ImageVariant>> cached(const std::string& source,
                                          const std::string& recipe,
                                          const std::string& name);
    std::unique_ptr<AttachmentDataSqlite> data;
    Configuration config;
    std::filesystem::path root;
    std::mutex mutation_mutex;
    std::mutex generation_mutex;
    std::condition_variable generation_condition;
    size_t active_conversions = 0;
    // Shared mutex ownership is required while a generation waiter exists.
    std::map<std::string, std::weak_ptr<std::mutex>> generation_locks;
};
