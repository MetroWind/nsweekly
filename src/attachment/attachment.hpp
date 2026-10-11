#pragma once
#include "error.hpp"
#include <filesystem>
#include <mw/utils.hpp>

// Public bytes and their immutable content identity.
struct AttachmentFile
{
    // Canonical content hash and content-probed media type.
    std::string hash, mime_type;
    // Exact stored bytes and first publication time in UTC Unix seconds.
    int64_t byte_size = 0, created = 0;
};
// An owner's named reference to public bytes.
struct Attachment
{
    // Stable record, owning account, and UTC upload time.
    int64_t id = 0, user_id = 0, added = 0;
    // Validated basename preserved without normalization.
    std::string original_name;
    // Immutable referenced byte sequence.
    AttachmentFile file;
};
// Successful publication, including an owner-local duplicate indication.
struct AttachmentUpload
{
    // Existing or newly created owner reference.
    Attachment record;
    // Encoded public URL; reading does not require ownership.
    std::string public_url;
    // True only when this owner already had the same hash and exact name.
    bool already_owned = false;
};
// Explicit bounded conversion policy supplied by a consumer.
struct ImageVariantOptions
{
    // Positive containment bounds and encoder quality in [1, 100].
    int max_width = 0, max_height = 0, quality = 80;
    // Explicit canonical output: jpeg, png, or webp.
    std::string format;
};
// A persisted reusable image result.
struct ImageVariant
{
    // Immutable referenced byte sequence.
    AttachmentFile file;
    // Actual output format and persistent public URL.
    std::string format, public_url;
    // Actual oriented output dimensions.
    int width = 0, height = 0;
};
// Accepts only canonical half-SHA-256 identifiers.
bool attachmentHash(std::string_view hash);
// Validates UTF-8 metadata and removes client path prefixes.
E<std::string> attachmentName(std::string_view name);
// Encodes an entire basename as one URL segment.
std::string attachmentSegment(std::string_view name);
// Constructs the public route for canonical bytes and a cosmetic name.
E<std::string> attachmentURL(std::string_view hash, std::string_view name);
// Derives the sole allowed published filesystem path.
E<std::filesystem::path> attachmentPath(const std::filesystem::path& root,
                                        std::string_view hash);
