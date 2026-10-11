#pragma once

#include <string>
#include <expected>
#include <filesystem>
#include <optional>
#include <cstdint>

#include "error.hpp"

// What should be displayed on the index page if there is no session?
enum class GuestIndex
{
    USER_WEEKLY,                // Redirect to the weekly of a user.
};

struct Configuration
{
    std::string data_dir;
    std::string listen_address;
    int listen_port;
    std::string client_id;
    std::string client_secret;
    std::string openid_url_prefix;
    std::string url_prefix;
    GuestIndex guest_index;
    std::string guest_index_user;
    std::string default_lang;

    // Public hash storage; an empty value defaults to data-dir/attachments.
    std::string attachment_dir;
    // Absent means no application byte cap, including for empty uploads.
    std::optional<uint64_t> attachment_upload_limit;
    // Conversion limits do not restrict storing original files.
    uint64_t attachment_image_pixels = 64000000;
    int attachment_image_dimension = 16384;
    size_t attachment_image_concurrency = 2;

    static E<Configuration> fromYaml(const std::filesystem::path& path);

};
