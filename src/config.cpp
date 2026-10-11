#include <string>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "config.hpp"
#include "error.hpp"

namespace {

E<std::vector<char>> readFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> content;
    content.reserve(102400);
    content.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());
    if(f.bad() || f.fail())
    {
        return std::unexpected(runtimeError(
            std::format("Failed to read file {}", path.string())));
    }

    return content;
}

template<class T>
bool getYamlValue(ryml::ConstNodeRef node, T& result)
{
    auto value = node.val();
    auto status = std::from_chars(value.begin(), value.end(), result);
    return status.ec == std::errc() && status.ptr == value.end();
}

} // namespace

E<Configuration> Configuration::fromYaml(const std::filesystem::path& path)
{
    auto buffer = readFile(path);
    if(!buffer.has_value())
    {
        return std::unexpected(buffer.error());
    }

    ryml::Tree tree = ryml::parse_in_place(ryml::to_substr(*buffer));
    Configuration config;
    if(tree["data-dir"].has_key())
    {
        auto value = tree["data-dir"].val();
        config.data_dir = std::string(value.begin(), value.end());
    }
    if(tree["listen-address"].has_key())
    {
        auto value = tree["listen-address"].val();
        config.listen_address = std::string(value.begin(), value.end());
    }
    if(tree["listen-port"].has_key())
    {
        if(!getYamlValue(tree["listen-port"], config.listen_port))
        {
            return std::unexpected(runtimeError("Invalid port"));
        }
    }
    if(tree["client-id"].has_key())
    {
        auto value = tree["client-id"].val();
        config.client_id = std::string(value.begin(), value.end());
    }
    if(tree["client-secret"].has_key())
    {
        auto value = tree["client-secret"].val();
        config.client_secret = std::string(value.begin(), value.end());
    }
    if(tree["openid-url-prefix"].has_key())
    {
        auto value = tree["openid-url-prefix"].val();
        config.openid_url_prefix = std::string(value.begin(), value.end());
    }
    if(tree["url-prefix"].has_key())
    {
        auto value = tree["url-prefix"].val();
        config.url_prefix = std::string(value.begin(), value.end());
    }
    if(tree["guest-index"].has_key())
    {
        auto value_bytes = tree["guest-index"].val();
        std::string value(value_bytes.begin(), value_bytes.end());
        if(value == "user-weekly")
        {
            config.guest_index = GuestIndex::USER_WEEKLY;
        }
        else
        {
            return std::unexpected(runtimeError("Invalid guest-index"));
        }
    }
    if(tree["guest-index-user"].has_key())
    {
        auto value = tree["guest-index-user"].val();
        config.guest_index_user = std::string(value.begin(), value.end());
    }
    if(tree["default-lang"].has_key())
    {
        auto value = tree["default-lang"].val();
        config.default_lang = std::string(value.begin(), value.end());
    }
    if(tree["attachment-dir"].has_key())
    {
        auto value = tree["attachment-dir"].val();
        config.attachment_dir.assign(value.begin(), value.end());
        if(config.attachment_dir.empty())
        { return std::unexpected(runtimeError("Empty attachment-dir")); }
    }
    if(tree["attachment-upload-limit"].has_key())
    {
        uint64_t value;
        if(!getYamlValue(tree["attachment-upload-limit"],value))
        { return std::unexpected(runtimeError("Invalid attachment-upload-limit")); }
        config.attachment_upload_limit = value;
    }
    if(tree["attachment-image-pixels"].has_key() &&
       !getYamlValue(tree["attachment-image-pixels"],config.attachment_image_pixels))
    { return std::unexpected(runtimeError("Invalid attachment-image-pixels")); }
    if(tree["attachment-image-dimension"].has_key() &&
       !getYamlValue(tree["attachment-image-dimension"],config.attachment_image_dimension))
    { return std::unexpected(runtimeError("Invalid attachment-image-dimension")); }
    if(tree["attachment-image-concurrency"].has_key() &&
       !getYamlValue(tree["attachment-image-concurrency"],config.attachment_image_concurrency))
    { return std::unexpected(runtimeError("Invalid attachment-image-concurrency")); }
    if(config.attachment_image_pixels == 0 ||
       config.attachment_image_pixels > 1000000000 ||
       config.attachment_image_dimension < 1 ||
       config.attachment_image_dimension > 65536 ||
       config.attachment_image_concurrency < 1 ||
       config.attachment_image_concurrency > 64)
    { return std::unexpected(runtimeError("Invalid attachment image limits")); }
    return E<Configuration>{std::in_place, std::move(config)};
}
