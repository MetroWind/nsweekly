#pragma once
#include "attachment/attachment.hpp"
#include <mw/crypto.hpp>

// Owns a staging file and incremental digest until publication completes.
class AttachmentStaging
{
public:
    // Creates an exclusive temporary name on the publication filesystem.
    static E<std::unique_ptr<AttachmentStaging>>
    create(const std::filesystem::path& root);
    // Removes the staging name and closes its writer on all paths.
    ~AttachmentStaging();
    AttachmentStaging(const AttachmentStaging&) = delete;
    AttachmentStaging& operator=(const AttachmentStaging&) = delete;
    // Appends exact binary bytes without buffering the file in memory.
    E<void> append(std::string_view bytes);
    // Fsyncs, closes, finalizes once, and probes content using libmagic.
    E<AttachmentFile> finish();
    // Exclusive staging path, also used by the image encoder.
    const std::filesystem::path& path() const
    {
        return temporary;
    }
    // Bytes accepted so far, independent of transport headers.
    int64_t size() const
    {
        return byte_size;
    }

private:
    AttachmentStaging(std::filesystem::path name, int descriptor)
        : temporary(std::move(name)), fd(descriptor)
    {
    }
    std::filesystem::path temporary;
    int fd = -1;
    int64_t byte_size = 0;
    bool failed = false;
    mw::SHA256HalfIncrementalHasher hasher;
};
// Probes a separate descriptor and falls back to octet-stream on failure.
std::string probeMimeType(const std::filesystem::path& path);
// Streams an existing file through libmw for recovery verification.
E<AttachmentFile> inspectAttachment(const std::filesystem::path& path);
// Atomically publishes without replacing an existing hash path; fsyncs shard.
E<void> publishAttachment(const std::filesystem::path& root,
                          const std::filesystem::path& staging,
                          const AttachmentFile& file);
