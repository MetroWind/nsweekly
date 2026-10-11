#pragma once
#include "attachment/attachment_data.hpp"
#include "attachment/attachment_files.hpp"
#include "config.hpp"
#include <condition_variable>
#include <map>
#include <mutex>

class AttachmentService;
// Move-only upload operation; destruction cancels unpublished bytes.
class AttachmentWriter
{
public:
    // Transfers an in-progress upload without copying its digest state.
    AttachmentWriter(AttachmentWriter&&) = default;
    AttachmentWriter& operator=(AttachmentWriter&&) = default;
    AttachmentWriter(const AttachmentWriter&) = delete;
    AttachmentWriter& operator=(const AttachmentWriter&) = delete;
    // Enforces the configured byte limit while writing a chunk.
    E<void> append(std::string_view bytes);
    // Publishes and creates or returns this owner's named record.
    E<AttachmentUpload> finish();

private:
    friend class AttachmentService;
    AttachmentWriter(AttachmentService& owner, int64_t user, std::string name,
                     std::unique_ptr<AttachmentStaging> temporary)
        : service(&owner), user_id(user), filename(std::move(name)),
          staging(std::move(temporary))
    {
    }
    AttachmentService* service;
    int64_t user_id;
    std::string filename;
    std::unique_ptr<AttachmentStaging> staging;
};

// Coordinates immutable public files, ownership, variants and reclamation.
class AttachmentService
{
public:
    // Takes a dedicated initialized SQLite connection and immutable settings.
    AttachmentService(std::unique_ptr<AttachmentDataSqlite> data,
                      const Configuration& settings);
    // Creates storage, removes interrupted work, and validates persisted bytes.
    E<void> initialize();
    // Validates storage identity and filename before accepting any file data.
    E<AttachmentWriter> beginUpload(int64_t user, std::string_view filename);
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
    friend class AttachmentWriter;
    E<AttachmentUpload> finish(AttachmentWriter& writer);
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
