#include "attachment/image_variant.hpp"
#include "attachment/srgb_profile.hpp"
#include <Magick++.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <unistd.h>

E<void> validateImageSettings(const Configuration& config)
{
    if(config.attachment_image_pixels == 0 ||
       config.attachment_image_pixels > 1000000000 ||
       config.attachment_image_dimension < 1 ||
       config.attachment_image_dimension > 65536 ||
       config.attachment_image_concurrency < 1 ||
       config.attachment_image_concurrency > 64)
    {
        return std::unexpected(runtimeError("Invalid attachment image limits"));
    }
    return {};
}
E<void> initializeImageConverter()
{
    static std::once_flag once;
    try
    {
        std::call_once(once,
                       []
                       {
                           Magick::InitializeMagick(nullptr);
                           Magick::ResourceLimits::memory(256ULL * 1024 * 1024);
                           Magick::ResourceLimits::map(512ULL * 1024 * 1024);
                           Magick::ResourceLimits::disk(1024ULL * 1024 * 1024);
                           Magick::ResourceLimits::thread(2);
                           Magick::ResourceLimits::listLength(2);
                       });
    }
    catch(const std::exception& error)
    {
        return std::unexpected(runtimeError(error.what()));
    }
    return {};
}
E<std::string> imageRecipe(const ImageVariantOptions& options,
                           const Configuration& config)
{
    DO_OR_RETURN(validateImageSettings(config));
    if(options.max_width < 1 || options.max_height < 1 ||
       options.max_width > config.attachment_image_dimension ||
       options.max_height > config.attachment_image_dimension ||
       options.quality < 1 || options.quality > 100 ||
       (options.format != "jpeg" && options.format != "png" &&
        options.format != "webp" && options.format != "avif" &&
        options.format != "jxl"))
    {
        return std::unexpected(httpError(422, "Invalid image options"));
    }
    return std::format("v2;metadata=preserve;fit=contain;upscale=0;"
                       "w={};h={};format={};q={}",
                       options.max_width, options.max_height, options.format,
                       options.quality);
}
E<std::pair<int, int>> convertImage(int descriptor, const std::string& mime,
                                    const ImageVariantOptions& options,
                                    const Configuration& config,
                                    AttachmentStaging& output)
{
    DO_OR_RETURN(imageRecipe(options, config));
    std::string coder = mime == "image/jpeg"   ? "JPEG"
                        : mime == "image/png"  ? "PNG"
                        : mime == "image/webp" ? "WEBP"
                                               : "";
    if(coder.empty())
    {
        return std::unexpected(httpError(422, "Unsupported image type"));
    }
    try
    {
        // An explicit raster coder prevents content from selecting delegates.
        const auto source =
            coder + ":/proc/self/fd/" + std::to_string(descriptor);
        std::list<Magick::Image> headers;
        Magick::ReadOptions probe;
        probe.ping(true);
        Magick::readImages(&headers, source, probe);
        if(headers.size() != 1)
        {
            return std::unexpected(
                httpError(422, "Multiframe images unsupported"));
        }
        auto valid = [&config](const Magick::Image& image)
        {
            return image.columns() > 0 && image.rows() > 0 &&
                   image.columns() <=
                       config.attachment_image_pixels / image.rows();
        };
        if(!valid(headers.front()))
        {
            return std::unexpected(httpError(422, "Image exceeds pixel limit"));
        }
        std::list<Magick::Image> images;
        Magick::readImages(&images, source);
        if(images.size() != 1 || !valid(images.front()))
        {
            return std::unexpected(
                httpError(422, "Image exceeds frame/pixel limit"));
        }
        auto image = std::move(images.front());
        if(image.magick() != "JPEG" && image.magick() != "PNG" &&
           image.magick() != "WEBP")
        {
            return std::unexpected(
                httpError(422, "Unsupported decoded format"));
        }
        const auto icc = image.profile("icc");
        if(icc.length() != 0)
        {
            // Reject malformed ICC containers before any encoder can discard
            // them. Force a profile transform on a copy to validate the
            // installed color-management delegate and the profile itself.
            if(icc.length() < 128 ||
               std::memcmp(static_cast<const char*>(icc.data()) + 36,
                           "acsp", 4) != 0)
            { return std::unexpected(httpError(422, "Invalid ICC profile")); }
            auto validation = image;
            validation.profile("icc", Magick::Blob(
                SRGB_PROFILE.data(), SRGB_PROFILE.size()));
        }
        image.autoOrient();
        image.orientation(Magick::TopLeftOrientation);
        double ratio = std::min(
            {1.0, static_cast<double>(options.max_width) / image.columns(),
             static_cast<double>(options.max_height) / image.rows()});
        if(ratio < 1)
        {
            Magick::Geometry geometry(
                std::max<size_t>(1, std::floor(image.columns() * ratio)),
                std::max<size_t>(1, std::floor(image.rows() * ratio)));
            geometry.aspect(true);
            image.resize(geometry);
        }
        if(options.format == "jpeg" && image.alpha())
        {
            // Composite in the source image's color interpretation, preserving
            // its profiles and metadata rather than copying into a new image.
            image.backgroundColor(Magick::Color("white"));
            image.alphaChannel(Magick::RemoveAlphaChannel);
        }
        image.magick(options.format);
        image.quality(options.quality);
        if(options.format == "png")
        {
            image.defineValue("png", "compression-level",
                              std::to_string((options.quality * 9 + 50) / 100));
        }
        // Encode to a temporary descriptor, then hash chunks through the same
        // staging writer as uploads; no whole-file buffer or shell converter.
        struct Temporary
        {
            FILE* file = tmpfile();
            ~Temporary()
            {
                if(file)
                {
                    fclose(file);
                }
            }
        } encoded;
        if(!encoded.file)
        {
            return std::unexpected(runtimeError("Cannot stage encoded image"));
        }
        image.write(options.format + ":/proc/self/fd/" +
                    std::to_string(fileno(encoded.file)));
        Magick::Image verified;
        verified.read(options.format + ":/proc/self/fd/" +
                      std::to_string(fileno(encoded.file)));
        if(icc.length() != 0 && options.format != "jxl")
        {
            const auto result_profile = verified.profile("icc");
            const bool identical = result_profile.length() == icc.length() &&
                std::memcmp(result_profile.data(), icc.data(), icc.length()) == 0;
            if(!identical)
            {
                return std::unexpected(httpError(422,
                    "Encoder cannot preserve the source color profile"));
            }
        }
        else if(icc.length() == 0 &&
                (image.colorSpace() != verified.colorSpace() ||
                 std::abs(image.gamma() - verified.gamma()) > 0.01))
        {
            return std::unexpected(httpError(422,
                "Encoder cannot preserve source color interpretation"));
        }
        if(fflush(encoded.file) != 0 || fseek(encoded.file, 0, SEEK_SET) != 0)
        {
            return std::unexpected(runtimeError("Cannot rewind encoded image"));
        }
        std::array<char, 65536> buffer;
        while(auto size = fread(buffer.data(), 1, buffer.size(), encoded.file))
        {
            DO_OR_RETURN(output.append({buffer.data(), size}));
        }
        if(ferror(encoded.file))
        {
            return std::unexpected(runtimeError("Cannot read encoded image"));
        }
        return std::pair<int, int>{image.columns(), image.rows()};
    }
    catch(const Magick::Exception& error)
    {
        return std::unexpected(httpError(
            422, std::string("Image conversion failed: ") + error.what()));
    }
}

E<void> validateImageEncoder(const ImageVariantOptions& options)
{
    DO_OR_RETURN(initializeImageConverter());
    const auto& format = options.format;
    try
    {
        Magick::CoderInfo coder(format);
        if(!coder.isWritable())
        { return std::unexpected(runtimeError("Unavailable encoder: " + format)); }
        Magick::Image image(Magick::Geometry(2, 2), Magick::Color("red"));
        image.magick(format);
        image.quality(options.quality);
        Magick::Blob encoded;
        image.write(&encoded, format);
        Magick::Image decoded(encoded);
        if(decoded.columns() != 2 || decoded.rows() != 2)
        { return std::unexpected(runtimeError("Invalid encoder: " + format)); }
    }
    catch(const Magick::Exception& error)
    {
        return std::unexpected(runtimeError(
            "Image encoder " + format + ": " + error.what()));
    }
    return {};
}
