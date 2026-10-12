#include "config.hpp"
#include "storage_test_support.hpp"
#include <fstream>
#include <gtest/gtest.h>

namespace
{
class ConfigurationParsing : public testing::Test
{
protected:
    E<Configuration> parse(const std::string& yaml)
    {
        const auto path = std::filesystem::path(temporary.path())
                              .replace_filename("configuration.yaml");
        {
            std::ofstream output(path);
            output << yaml;
        }
        return Configuration::fromYaml(path);
    }
    TemporaryDatabase temporary;
};
} // namespace

TEST_F(ConfigurationParsing, LegacyConfigUsesAttachmentDefaults)
{
    auto config = parse("data-dir: /tmp/nsweekly\n"
                        "listen-address: localhost\nlisten-port: 58787\n"
                        "client-id: test\nclient-secret: test\n"
                        "openid-url-prefix: https://example.com\n"
                        "url-prefix: https://weekly.example.com\n"
                        "guest-index: user-weekly\nguest-index-user: mw\n"
                        "default-lang: en-US\n");
    ASSERT_TRUE(config) << errorMsg(config.error());
    EXPECT_TRUE(config->attachment_dir.empty());
    EXPECT_FALSE(config->attachment_upload_limit);
    EXPECT_EQ(config->attachment_image_pixels, 64000000);
    EXPECT_EQ(config->attachment_image_dimension, 16384);
    EXPECT_EQ(config->attachment_image_concurrency, 2);
}

TEST_F(ConfigurationParsing, MissingKeysNeverReadUninitializedNodes)
{
    auto config = parse("{}\n");
    ASSERT_TRUE(config) << errorMsg(config.error());
    EXPECT_EQ(config->listen_port, 0);
    EXPECT_TRUE(config->attachment_dir.empty());
    config = parse("attachment-upload-limit: 0\n");
    ASSERT_TRUE(config) << errorMsg(config.error());
    ASSERT_TRUE(config->attachment_upload_limit);
    EXPECT_EQ(*config->attachment_upload_limit, 0);
}

TEST_F(ConfigurationParsing, ExplicitAttachmentSettingsArePreserved)
{
    auto config =
        parse("attachment-dir: /tmp/custom\nattachment-upload-limit: 52428800\n"
              "attachment-image-pixels: 1000000\n"
              "attachment-image-dimension: 2048\n"
              "attachment-image-concurrency: 4\n");
    ASSERT_TRUE(config) << errorMsg(config.error());
    EXPECT_EQ(config->attachment_dir, "/tmp/custom");
    ASSERT_TRUE(config->attachment_upload_limit);
    EXPECT_EQ(*config->attachment_upload_limit, 52428800);
    EXPECT_EQ(config->attachment_image_pixels, 1000000);
    EXPECT_EQ(config->attachment_image_dimension, 2048);
    EXPECT_EQ(config->attachment_image_concurrency, 4);
}

TEST_F(ConfigurationParsing, RejectsInvalidOptionalSettingsWithoutAborting)
{
    for(const auto yaml :
        {"attachment-dir: ''", "attachment-dir:",
         "attachment-upload-limit: null", "attachment-upload-limit: -1",
         "attachment-upload-limit: 12bytes", "attachment-upload-limit: []",
         "attachment-image-pixels: 0", "attachment-image-dimension: 65537",
         "attachment-image-concurrency: 0", "attachment-image-concurrency: 65",
         "listen-port: {}", "[]", "plain scalar", ""})
    {
        EXPECT_FALSE(parse(yaml)) << yaml;
    }
}

TEST_F(ConfigurationParsing, ScreenshotDefaultsAliasesAndBounds)
{
    auto defaults = parse("{}");
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->screenshot_thumbnail.long_edge, 320);
    EXPECT_EQ(defaults->screenshot_preview.quality, 85);
    for(auto alias : {"jpg", "jpeg", "webp", "avif", "jxl", "jpegxl"})
    {
        auto config = parse(std::string("game-screenshots:\n  thumbnail:\n") +
            "    codec: " + alias + "\n    quality: 100\n    long-edge: 240\n");
        ASSERT_TRUE(config) << alias << errorMsg(config.error());
        EXPECT_EQ(config->screenshot_thumbnail.codec, *screenshotCodec(alias));
        EXPECT_EQ(config->screenshot_thumbnail.quality, 100);
    }
    for(auto invalid : {"null", "[]", "{thumbnail: null}",
        "{thumbnail: {codec: png}}", "{thumbnail: {codec: {}}}",
        "{thumbnail: {quality: 1.5}}", "{preview: {quality: 101}}",
        "{thumbnail: {quality: 0}}", "{preview: {long-edge: -1}}",
        "{preview: {long-edge: 20000}}", "{preview: {long-edge: 100}}",
        "{thumbnail: {long-edge: []}}"})
    {
        EXPECT_FALSE(parse(std::string("game-screenshots: ") + invalid));
    }
}
