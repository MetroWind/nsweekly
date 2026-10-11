#include "attachment/attachment_files.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <magic.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
E<void> fileError(std::string_view operation)
{
    return std::unexpected(
        runtimeError(std::string(operation) + ": " + std::strerror(errno)));
}
struct Descriptor
{
    int fd;
    ~Descriptor()
    {
        if(fd >= 0)
        {
            close(fd);
        }
    }
};
struct Magic
{
    magic_t handle = magic_open(MAGIC_MIME_TYPE);
    ~Magic()
    {
        if(handle)
        {
            magic_close(handle);
        }
    }
};
} // namespace

std::string probeMimeType(const std::filesystem::path& path)
{
    Magic magic;
    Descriptor input{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if(!magic.handle || input.fd < 0 || magic_load(magic.handle, nullptr) != 0)
    {
        return "application/octet-stream";
    }
    const char* type = magic_descriptor(magic.handle, input.fd);
    return type ? std::string(type) : "application/octet-stream";
}

E<std::unique_ptr<AttachmentStaging>>
AttachmentStaging::create(const std::filesystem::path& root)
{
    std::string pattern = (root / ".staging" / "upload_XXXXXX").string();
    int descriptor = mkstemp(pattern.data());
    if(descriptor < 0)
    {
        return std::unexpected(
            runtimeError("Cannot create attachment staging file"));
    }
    return std::unique_ptr<AttachmentStaging>(
        new AttachmentStaging(pattern, descriptor));
}
AttachmentStaging::~AttachmentStaging()
{
    if(fd >= 0)
    {
        close(fd);
    }
    std::error_code error;
    std::filesystem::remove(temporary, error);
}
E<void> AttachmentStaging::append(std::string_view bytes)
{
    if(fd < 0 || failed)
    {
        return std::unexpected(runtimeError("Staging writer is closed"));
    }
    failed = true;
    if(bytes.size() > static_cast<uint64_t>(INT64_MAX - byte_size))
    {
        return std::unexpected(runtimeError("Attachment size overflow"));
    }
    DO_OR_RETURN(hasher.update(bytes));
    size_t offset = 0;
    while(offset < bytes.size())
    {
        auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if(count < 0 && errno == EINTR)
        {
            continue;
        }
        if(count <= 0)
        {
            return fileError("Writing attachment");
        }
        offset += count;
    }
    byte_size += bytes.size();
    failed = false;
    return {};
}
E<AttachmentFile> AttachmentStaging::finish()
{
    if(fd < 0 || failed)
    {
        return std::unexpected(runtimeError("Staging writer is closed"));
    }
    failed = true;
    DO_OR_RETURN(fsync(fd) == 0 ? E<void>{} : fileError("Sync attachment"));
    int descriptor = std::exchange(fd, -1);
    DO_OR_RETURN(close(descriptor) == 0 ? E<void>{}
                                        : fileError("Close attachment"));
    ASSIGN_OR_RETURN(auto hash, hasher.finalizeToHexStr());
    return AttachmentFile{hash, probeMimeType(temporary), byte_size,
                          static_cast<int64_t>(time(nullptr))};
}
E<AttachmentFile> inspectAttachment(const std::filesystem::path& path)
{
    Descriptor input{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat info{};
    if(input.fd < 0 || fstat(input.fd, &info) != 0 || !S_ISREG(info.st_mode))
    {
        return std::unexpected(runtimeError("Cannot inspect attachment file"));
    }
    mw::SHA256HalfIncrementalHasher hasher;
    std::array<char, 65536> buffer;
    int64_t size = 0;
    while(true)
    {
        auto count = read(input.fd, buffer.data(), buffer.size());
        if(count < 0 && errno == EINTR)
        {
            continue;
        }
        if(count < 0)
        {
            return std::unexpected(runtimeError("Cannot read attachment file"));
        }
        if(count == 0)
        {
            break;
        }
        DO_OR_RETURN(
            hasher.update({buffer.data(), static_cast<size_t>(count)}));
        size += count;
    }
    ASSIGN_OR_RETURN(auto hash, hasher.finalizeToHexStr());
    return AttachmentFile{hash, probeMimeType(path), size,
                          static_cast<int64_t>(time(nullptr))};
}
E<void> publishAttachment(const std::filesystem::path& root,
                          const std::filesystem::path& staging,
                          const AttachmentFile& file)
{
    ASSIGN_OR_RETURN(auto path, attachmentPath(root, file.hash));
    std::error_code error;
    bool created = std::filesystem::create_directory(path.parent_path(), error);
    if(error)
    {
        return std::unexpected(runtimeError(error.message()));
    }
    if(created)
    {
        Descriptor directory{open(root.c_str(), O_RDONLY | O_DIRECTORY)};
        if(directory.fd < 0 || fsync(directory.fd) != 0)
        {
            return fileError("Sync attachment root");
        }
    }
    if(link(staging.c_str(), path.c_str()) != 0)
    {
        if(errno != EEXIST)
        {
            return fileError("Publish attachment");
        }
        ASSIGN_OR_RETURN(auto existing, inspectAttachment(path));
        if(existing.hash != file.hash)
        {
            return std::unexpected(runtimeError("Corrupt orphan attachment"));
        }
    }
    // Published files must also be readable by the static file server.
    if(chmod(path.c_str(), 0644) != 0)
    {
        return fileError("Attachment permissions");
    }
    Descriptor directory{
        open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY)};
    if(directory.fd < 0 || fsync(directory.fd) != 0)
    {
        return fileError("Sync attachment shard");
    }
    return {};
}
