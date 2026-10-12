#include <string>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "config.hpp"
#include "error.hpp"
#include <mw/utils.hpp>

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
    if(!node.has_val() || node.val_is_null())
    {
        return false;
    }
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
    if(tree.empty() || !tree.crootref().is_map())
    {
        return std::unexpected(runtimeError("Configuration must be a YAML map"));
    }
    const auto root = tree.crootref();
    // Check membership before indexing: a missing NodeRef is unreadable.
    for(const auto child: root.children())
    {
        if(child.key() != "game-screenshots" &&
           (!child.has_val() || child.val_is_null()))
        {
            return std::unexpected(runtimeError(
                "Configuration values must be non-null scalars"));
        }
    }
    Configuration config{};
    if(root.has_child("data-dir"))
    {
        auto value = root["data-dir"].val();
        config.data_dir = std::string(value.begin(), value.end());
    }
    if(root.has_child("listen-address"))
    {
        auto value = root["listen-address"].val();
        config.listen_address = std::string(value.begin(), value.end());
    }
    if(root.has_child("listen-port"))
    {
        if(!getYamlValue(root["listen-port"], config.listen_port))
        {
            return std::unexpected(runtimeError("Invalid port"));
        }
    }
    if(root.has_child("client-id"))
    {
        auto value = root["client-id"].val();
        config.client_id = std::string(value.begin(), value.end());
    }
    if(root.has_child("client-secret"))
    {
        auto value = root["client-secret"].val();
        config.client_secret = std::string(value.begin(), value.end());
    }
    if(root.has_child("openid-url-prefix"))
    {
        auto value = root["openid-url-prefix"].val();
        config.openid_url_prefix = std::string(value.begin(), value.end());
    }
    if(root.has_child("url-prefix"))
    {
        auto value = root["url-prefix"].val();
        config.url_prefix = std::string(value.begin(), value.end());
    }
    if(root.has_child("guest-index"))
    {
        auto value_bytes = root["guest-index"].val();
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
    if(root.has_child("guest-index-user"))
    {
        auto value = root["guest-index-user"].val();
        config.guest_index_user = std::string(value.begin(), value.end());
    }
    if(root.has_child("default-lang"))
    {
        auto value = root["default-lang"].val();
        config.default_lang = std::string(value.begin(), value.end());
    }
    if(root.has_child("attachment-dir"))
    {
        auto value = root["attachment-dir"].val();
        config.attachment_dir.assign(value.begin(), value.end());
        if(config.attachment_dir.empty())
        { return std::unexpected(runtimeError("Empty attachment-dir")); }
    }
    if(root.has_child("attachment-upload-limit"))
    {
        uint64_t value;
        if(!getYamlValue(root["attachment-upload-limit"],value))
        { return std::unexpected(runtimeError("Invalid attachment-upload-limit")); }
        config.attachment_upload_limit = value;
    }
    if(root.has_child("attachment-image-pixels") &&
       !getYamlValue(root["attachment-image-pixels"],config.attachment_image_pixels))
    { return std::unexpected(runtimeError("Invalid attachment-image-pixels")); }
    if(root.has_child("attachment-image-dimension") &&
       !getYamlValue(root["attachment-image-dimension"],config.attachment_image_dimension))
    { return std::unexpected(runtimeError("Invalid attachment-image-dimension")); }
    if(root.has_child("attachment-image-concurrency") &&
       !getYamlValue(root["attachment-image-concurrency"],config.attachment_image_concurrency))
    { return std::unexpected(runtimeError("Invalid attachment-image-concurrency")); }
    if(config.attachment_image_pixels == 0 ||
       config.attachment_image_pixels > 1000000000 ||
       config.attachment_image_dimension < 1 ||
       config.attachment_image_dimension > 65536 ||
       config.attachment_image_concurrency < 1 ||
       config.attachment_image_concurrency > 64)
    { return std::unexpected(runtimeError("Invalid attachment image limits")); }
    if(root.has_child("game-screenshots"))
    {
        auto screenshots = root["game-screenshots"];
        if(!screenshots.is_map())
        {
            return std::unexpected(runtimeError("Invalid game-screenshots"));
        }
        for(auto [name, settings] : {
            std::pair{"thumbnail", &config.screenshot_thumbnail},
            std::pair{"preview", &config.screenshot_preview}})
        {
            if(!screenshots.has_child(name)) { continue; }
            auto node = screenshots[name];
            const auto field = std::string("game-screenshots.") + name;
            if(!node.is_map())
            { return std::unexpected(runtimeError("Invalid " + field)); }
            if(node.has_child("codec"))
            {
                auto codec = node["codec"];
                if(!codec.has_val() || codec.val_is_null())
                { return std::unexpected(runtimeError(field + ".codec")); }
                auto value = codec.val();
                auto parsed = screenshotCodec({value.begin(), value.len});
                if(!parsed)
                { return std::unexpected(runtimeError(field + ".codec")); }
                settings->codec = *parsed;
            }
            for(auto [key, value] : {
                std::pair{"quality", &settings->quality},
                std::pair{"long-edge", &settings->long_edge}})
            {
                if(node.has_child(key) && !getYamlValue(node[key], *value))
                { return std::unexpected(runtimeError(field + "." + key)); }
            }
        }
    }
    DO_OR_RETURN(validateScreenshotSettings(config));
    return E<Configuration>{std::in_place, std::move(config)};
}

std::string screenshotFormat(ScreenshotCodec codec)
{
    switch(codec)
    {
    case ScreenshotCodec::JPEG: return "jpeg";
    case ScreenshotCodec::WEBP: return "webp";
    case ScreenshotCodec::AVIF: return "avif";
    case ScreenshotCodec::JXL: return "jxl";
    }
    return {};
}

E<ScreenshotCodec> screenshotCodec(std::string_view name)
{
    if(name == "jpg" || name == "jpeg") { return ScreenshotCodec::JPEG; }
    if(name == "webp") { return ScreenshotCodec::WEBP; }
    if(name == "avif") { return ScreenshotCodec::AVIF; }
    if(name == "jxl" || name == "jpegxl") { return ScreenshotCodec::JXL; }
    return std::unexpected(runtimeError("Unsupported screenshot codec"));
}

E<void> validateScreenshotSettings(const Configuration& config)
{
    for(auto [name, settings] : {
        std::pair{"thumbnail", config.screenshot_thumbnail},
        std::pair{"preview", config.screenshot_preview}})
    {
        auto field = std::string("game-screenshots.") + name;
        if(screenshotFormat(settings.codec).empty())
        { return std::unexpected(runtimeError(field + ".codec")); }
        if(settings.quality < 1 || settings.quality > 100)
        { return std::unexpected(runtimeError(field + ".quality")); }
        if(settings.long_edge < 1 ||
           settings.long_edge > config.attachment_image_dimension)
        { return std::unexpected(runtimeError(field + ".long-edge")); }
    }
    if(config.screenshot_thumbnail.long_edge >
       config.screenshot_preview.long_edge)
    {
        return std::unexpected(runtimeError(
            "game-screenshots.thumbnail.long-edge exceeds preview.long-edge"));
    }
    return {};
}
