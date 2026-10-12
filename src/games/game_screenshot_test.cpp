#include "games/game_screenshot_service.hpp"
#include "games/game_screenshot_data.hpp"
#include "attachment/attachment_module.hpp"
#include "attachment/image_variant.hpp"
#include "auth/auth_mock.hpp"
#include "games/games_module.hpp"
#include "storage_test_support.hpp"
#include "test_utils.hpp"
#include "user_sql.hpp"
#include <Magick++.h>
#include <fstream>
#include <future>
#include <thread>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace
{
std::string readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
} // namespace

class Screenshots : public testing::Test
{
protected:
    TemporaryDatabase temporary;
    Configuration config{};
    std::unique_ptr<SQLite> inspection;
    std::unique_ptr<GameDataSqlite> games;
    std::unique_ptr<AttachmentService> attachments;
    std::unique_ptr<GameScreenshotService> service;
    int64_t game_id = 0, other_game = 0;
    void SetUp() override
    {
        config.data_dir = std::filesystem::path(temporary.path()).parent_path();
        config.screenshot_thumbnail.long_edge = 8;
        config.screenshot_preview.long_edge = 16;
        config.url_prefix = "http://localhost";
        ASSIGN_OR_FAIL(inspection, SQLite::connectFile(temporary.path()));
        ASSERT_TRUE(initializeUsers(*inspection));
        ASSERT_TRUE(inspection->execute(
            "INSERT INTO Users(id,name) VALUES(1,'one'),(2,'two')"));
        ASSIGN_OR_FAIL(auto game_db, SQLite::connectFile(temporary.path()));
        games = std::make_unique<GameDataSqlite>(std::move(game_db));
        ASSERT_TRUE(games->initializeSchema());
        GameInput input;
        input.name = "Mountains";
        ASSIGN_OR_FAIL(auto record, games->createGame("one", input));
        game_id = record.id;
        input.name = "Ocean";
        ASSIGN_OR_FAIL(record, games->createGame("one", input));
        other_game = record.id;
        restart();
    }
    void restart()
    {
        service.reset();
        attachments.reset();
        ASSIGN_OR_FAIL(auto db, SQLite::connectFile(temporary.path()));
        attachments = std::make_unique<AttachmentService>(
            std::make_unique<AttachmentDataSqlite>(std::move(db)), config);
        ASSERT_TRUE(attachments->initialize());
        service = std::make_unique<GameScreenshotService>(*attachments, *games,
                                                          config);
        auto ready = service->initialize();
        ASSERT_TRUE(ready) << errorMsg(ready.error());
    }
    std::string image()
    {
        Magick::Image pixels(Magick::Geometry(40, 20), Magick::Color("red"));
        Magick::Blob bytes;
        pixels.write(&bytes, "PNG");
        return {static_cast<const char*>(bytes.data()), bytes.length()};
    }
    E<GameScreenshot> upload(int64_t game, std::string_view name,
                             std::string_view bytes)
    {
        ASSIGN_OR_RETURN(auto writer,
                         service->beginUpload("one", 1, game, name));
        DO_OR_RETURN(writer.append(bytes));
        return writer.finish();
    }
    int64_t count(const std::string& table)
    {
        auto rows = inspection->eval<int64_t>("SELECT count(*) FROM " + table);
        EXPECT_TRUE(rows);
        return rows ? std::get<0>(rows->at(0)) : -1;
    }
};

TEST_F(Screenshots, PublishesTypedRecipesAndPreservesOriginal)
{
    auto bytes = image();
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "山 & moon.png", bytes));
    EXPECT_EQ(shot.thumbnail.width, 8);
    EXPECT_EQ(shot.thumbnail.height, 4);
    EXPECT_EQ(shot.preview.width, 16);
    EXPECT_EQ(shot.thumbnail_settings.codec, ScreenshotCodec::WEBP);
    EXPECT_EQ(shot.thumbnail_settings.quality, 75);
    EXPECT_EQ(shot.preview_settings.quality, 85);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    auto path =
        *attachmentPath(attachments->directory(), shot.attachment.file.hash);
    EXPECT_EQ(readBytes(path), bytes);
    ASSIGN_OR_FAIL(
        auto stored,
        (inspection->eval<int, int, int, int, int, int>(
            "SELECT thumbnail_codec,thumbnail_size,thumbnail_quality,"
            "preview_codec,preview_size,preview_quality FROM "
            "GameScreenshots")));
    EXPECT_EQ(stored.at(0), (std::tuple{1, 8, 75, 1, 16, 85}));
    config.screenshot_thumbnail.quality = 90;
    ASSIGN_OR_FAIL(auto retry, upload(game_id, "山 & moon.png", bytes));
    EXPECT_TRUE(retry.already_uploaded);
    EXPECT_EQ(retry.id, shot.id);
    EXPECT_EQ(retry.thumbnail_settings.quality, 75);
    EXPECT_EQ(count("AttachmentVariants"), 2);
    EXPECT_FALSE(upload(other_game, "山 & moon.png", bytes));
    EXPECT_EQ(count("Attachments"), 1);
    EXPECT_EQ(readBytes(path), bytes);
}

TEST_F(Screenshots, FailedNewUploadIsReleasedButExistingAttachmentSurvives)
{
    EXPECT_FALSE(upload(game_id, "broken.png", "not an image"));
    EXPECT_EQ(count("Attachments"), 0);
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(1, "existing.png"));
    ASSERT_TRUE(writer.append("not an image"));
    ASSIGN_OR_FAIL(auto existing, writer.finish());
    EXPECT_FALSE(upload(game_id, "existing.png", "not an image"));
    EXPECT_TRUE(attachments->getAttachment(1, existing.record.id));
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
}

TEST_F(Screenshots, FailureBetweenVariantsReclaimsNewUpload)
{
    ASSERT_TRUE(inspection->execute(R"(
CREATE TRIGGER FailPreview BEFORE INSERT ON AttachmentVariants
 WHEN NEW.width=16 BEGIN SELECT RAISE(ABORT,'injected failure'); END)"));
    EXPECT_FALSE(upload(game_id, "new.png", image()));
    EXPECT_EQ(count("Attachments"), 0);
    EXPECT_EQ(count("AttachmentVariants"), 0);
    EXPECT_EQ(count("AttachmentFiles"), 0);
    EXPECT_EQ(count("GameScreenshots"), 0);
}

TEST_F(Screenshots, DeletionCascadeQueueRollbackAndRestart)
{
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "new.png", image()));
    ASSERT_TRUE(inspection->execute("BEGIN IMMEDIATE"));
    ASSERT_TRUE(inspection->execute("DELETE FROM GameTracking WHERE id=" +
                                    std::to_string(game_id)));
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("GameScreenshotCleanup"), 1);
    ASSERT_TRUE(inspection->execute("ROLLBACK"));
    EXPECT_EQ(count("GameScreenshots"), 1);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    // Simulate crash after the game's commit and before service cleanup.
    ASSERT_TRUE(games->deleteGame("one", game_id));
    EXPECT_EQ(count("GameScreenshotCleanup"), 1);
    restart();
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    EXPECT_EQ(count("Attachments"), 0);
    EXPECT_EQ(count("AttachmentFiles"), 0);
    EXPECT_FALSE(std::filesystem::exists(
        *attachmentPath(attachments->directory(), shot.attachment.file.hash)));
}

TEST_F(Screenshots, DirectAttachmentDeletionCascadesWithoutStaleQueue)
{
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "new.png", image()));
    ASSERT_TRUE(attachments->deleteAttachment(1, shot.attachment.id));
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    EXPECT_EQ(count("AttachmentFiles"), 0);
}

TEST_F(Screenshots, ScreenshotDeletionKeepsSharedPhysicalBytes)
{
    auto bytes = image();
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "new.png", bytes));
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(2, "other.png"));
    ASSERT_TRUE(writer.append(bytes));
    ASSIGN_OR_FAIL(auto other, writer.finish());
    ASSERT_TRUE(service->remove(1, game_id, shot.id));
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("Attachments"), 1);
    EXPECT_EQ(count("AttachmentVariants"), 2);
    auto path =
        *attachmentPath(attachments->directory(), shot.attachment.file.hash);
    EXPECT_EQ(readBytes(path), bytes);
    ASSERT_TRUE(attachments->deleteAttachment(2, other.record.id));
    EXPECT_EQ(count("AttachmentFiles"), 0);
}

TEST_F(Screenshots, AbandonedReceptionNeverPublishesOrEntersDeletionQueue)
{
    {
        ASSIGN_OR_FAIL(auto writer, service->beginUpload("one", 1, game_id,
                                                         "interrupted.png"));
        ASSERT_TRUE(writer.append(image()));
        EXPECT_EQ(count("Attachments"), 0);
        EXPECT_EQ(count("GameScreenshotCleanup"), 0);
        ASSERT_TRUE(attachments->cleanup());
    }
    ASSIGN_OR_FAIL(auto ordinary, attachments->beginUpload(1, "ordinary"));
    ASSERT_TRUE(ordinary.append("abc"));
    ASSIGN_OR_FAIL(auto kept, ordinary.finish());
    restart();
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    EXPECT_EQ(count("Attachments"), 1);
    EXPECT_TRUE(attachments->getAttachment(1, kept.record.id));
}

TEST_F(Screenshots, RejectsOwnerMismatchAndImmutableUpdatesAtDatabaseBoundary)
{
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "one.png", image()));
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(2, "two.png"));
    ASSERT_TRUE(writer.append(image()));
    ASSIGN_OR_FAIL(auto other, writer.finish());
    EXPECT_FALSE(inspection->execute(
        "INSERT INTO GameScreenshots(game_id,attachment_id,thumbnail_codec,"
        "thumbnail_size,thumbnail_quality,preview_codec,preview_size,"
        "preview_quality,added) VALUES(" +
        std::to_string(other_game) + "," + std::to_string(other.record.id) +
        ",1,8,75,1,16,85,0)"));
    EXPECT_FALSE(inspection->execute("UPDATE GameScreenshots SET added=1"));
    EXPECT_EQ(count("GameScreenshots"), 1);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
}

TEST_F(Screenshots, CleanupAndGameDeletionWaitForActiveUpload)
{
    ASSIGN_OR_FAIL(auto writer,
                   service->beginUpload("one", 1, game_id, "new.png"));
    ASSERT_TRUE(writer.append(image()));
    auto deletion = std::async(std::launch::async, [&]
                               { return service->deleteGame("one", game_id); });
    EXPECT_EQ(deletion.wait_for(std::chrono::milliseconds(20)),
              std::future_status::timeout);
    ASSERT_TRUE(writer.finish());
    EXPECT_TRUE(deletion.get());
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("Attachments"), 0);
}

class ScreenshotCodecs : public Screenshots,
                         public testing::WithParamInterface<ScreenshotCodec>
{
};

TEST_P(ScreenshotCodecs, EncodedSignatureMetadataProfilesColorsAndOriginals)
{
    const auto codec = GetParam();
    const auto format = screenshotFormat(codec);
    auto settings = config;
    settings.screenshot_thumbnail.codec = codec;
    settings.screenshot_preview.codec = codec;
    auto available = validateScreenshotEncoders(settings);
    if(!available)
    {
        if(codec == ScreenshotCodec::WEBP)
        {
            FAIL() << errorMsg(available.error());
        }
        GTEST_SKIP() << "Optional codec unavailable: " << format << ": "
                     << errorMsg(available.error());
    }
    for(const auto name : {"srgb", "display_p3", "adobe_rgb"})
    {
        auto profile =
            readBytes(std::filesystem::path(NSWEEKLY_SOURCE_DIR) /
                      "tests/fixtures/color" / (std::string(name) + ".icc"));
        ASSERT_FALSE(profile.empty());
        Magick::Image source(Magick::Geometry(24, 12),
                             Magick::ColorRGB(.75, .3, .2));
        source.depth(8);
        source.profile("icc", Magick::Blob(profile.data(), profile.size()));
        source.comment("A public screenshot comment");
        source.profile(
            "xmp", Magick::Blob("<x:xmpmeta xmlns:x='adobe:ns:meta/'/>", 35));
        Magick::Blob blob;
        source.write(&blob, "PNG");
        std::string bytes(static_cast<const char*>(blob.data()), blob.length());
        ASSIGN_OR_FAIL(auto writer,
                       attachments->beginUpload(1, std::string(name) + ".png"));
        ASSERT_TRUE(writer.append(bytes));
        ASSIGN_OR_FAIL(auto upload, writer.finish());
        ASSIGN_OR_FAIL(auto variant,
                       attachments->ensureImageVariant(1, upload.record.id,
                                                       {8, 8, 95, format}));
        auto path =
            *attachmentPath(attachments->directory(), variant.file.hash);
        auto encoded = readBytes(path);
        ASSERT_FALSE(encoded.empty());
        if(codec == ScreenshotCodec::JPEG)
        {
            EXPECT_EQ(encoded.substr(0, 2), std::string("\xff\xd8", 2));
        }
        if(codec == ScreenshotCodec::WEBP)
        {
            EXPECT_EQ(encoded.substr(0, 4), "RIFF");
            EXPECT_EQ(encoded.substr(8, 4), "WEBP");
        }
        if(codec == ScreenshotCodec::AVIF)
        {
            EXPECT_NE(encoded.find("ftypavif"), std::string::npos);
        }
        if(codec == ScreenshotCodec::JXL)
        {
            EXPECT_TRUE(encoded.starts_with(std::string("\xff\x0a", 2)) ||
                        encoded.starts_with(std::string("\0\0\0\x0cJXL ", 8)));
        }
        Magick::Image decoded(path.string());
        auto retained = decoded.profile("icc");
        if(codec != ScreenshotCodec::JXL)
        {
            EXPECT_EQ(retained.length(), profile.size());
            EXPECT_EQ(std::string(static_cast<const char*>(retained.data()),
                                  retained.length()),
                      profile);
        }
        // JXL profile preservation is deferred; signatures and decode still
        // run for that codec. The other codecs must preserve managed colors.
        if(codec != ScreenshotCodec::JXL)
        {
            // Compare the decoded pixels in the same color-managed sRGB space.
            auto srgb = readBytes(std::filesystem::path(NSWEEKLY_SOURCE_DIR) /
                                  "tests/fixtures/color/srgb.icc");
            decoded.profile("icc", Magick::Blob(srgb.data(), srgb.size()));
            source.profile("icc", Magick::Blob(srgb.data(), srgb.size()));
            Magick::ColorRGB actual(decoded.pixelColor(3, 2));
            Magick::ColorRGB reference(source.pixelColor(3, 2));
            EXPECT_NEAR(actual.red(), reference.red(), .06) << name;
            EXPECT_NEAR(actual.green(), reference.green(), .06) << name;
            EXPECT_NEAR(actual.blue(), reference.blue(), .06) << name;
            auto trusted = nlohmann::json::parse(
                readBytes(std::filesystem::path(NSWEEKLY_SOURCE_DIR) /
                          "tests/fixtures/color/reference.json"))[name];
            EXPECT_NEAR(actual.red(), trusted[0].get<double>() / 255, .06);
            EXPECT_NEAR(actual.green(), trusted[1].get<double>() / 255, .06);
            EXPECT_NEAR(actual.blue(), trusted[2].get<double>() / 255, .06);
        }
        EXPECT_EQ(readBytes(*attachmentPath(attachments->directory(),
                                            upload.record.file.hash)),
                  bytes);
    }
}

TEST_P(ScreenshotCodecs, UnprofiledOrientationTransparencyAndNoEnlargement)
{
    auto settings = config;
    settings.screenshot_thumbnail.codec = GetParam();
    settings.screenshot_preview.codec = GetParam();
    auto available = validateScreenshotEncoders(settings);
    if(!available)
    {
        if(GetParam() == ScreenshotCodec::WEBP)
        {
            FAIL();
        }
        GTEST_SKIP() << errorMsg(available.error());
    }
    Magick::Image pixels(Magick::Geometry(5, 10), Magick::Color("none"));
    pixels.pixelColor(2, 3, Magick::Color("red"));
    Magick::Blob blob;
    pixels.write(&blob, "PNG");
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(1, "alpha.png"));
    ASSERT_TRUE(
        writer.append({static_cast<const char*>(blob.data()), blob.length()}));
    ASSIGN_OR_FAIL(auto upload, writer.finish());
    ASSIGN_OR_FAIL(
        auto variant,
        attachments->ensureImageVariant(
            1, upload.record.id, {16, 16, 90, screenshotFormat(GetParam())}));
    EXPECT_EQ(variant.width, 5);
    EXPECT_EQ(variant.height, 10);
    Magick::Image decoded(
        attachmentPath(attachments->directory(), variant.file.hash)->string());
    if(GetParam() == ScreenshotCodec::JPEG)
    {
        EXPECT_FALSE(decoded.alpha());
        Magick::ColorRGB white(decoded.pixelColor(0, 0));
        EXPECT_GT(white.red(), .9);
        EXPECT_GT(white.green(), .9);
        EXPECT_GT(white.blue(), .9);
    }
    else
    {
        EXPECT_TRUE(decoded.alpha());
    }
}

INSTANTIATE_TEST_SUITE_P(All, ScreenshotCodecs,
                         testing::Values(ScreenshotCodec::JPEG,
                                         ScreenshotCodec::WEBP,
                                         ScreenshotCodec::AVIF,
                                         ScreenshotCodec::JXL));

class ScreenshotHTTP : public Screenshots
{
protected:
    testing::NiceMock<AuthMock> auth;
    std::unique_ptr<UserDataSqlite> users;
    std::unique_ptr<SessionService> sessions;
    std::unique_ptr<GamesModule> module;
    std::unique_ptr<AttachmentModule> files;
    httplib::Server server;
    std::thread thread;
    std::unique_ptr<httplib::Client> client;
    std::string gallery;
    void SetUp() override
    {
        Screenshots::SetUp();
        config.data_dir = NSWEEKLY_SOURCE_DIR;
        ASSIGN_OR_FAIL(auto db, SQLite::connectFile(temporary.path()));
        users = std::make_unique<UserDataSqlite>(std::move(db));
        sessions = std::make_unique<SessionService>(auth);
        module = std::make_unique<GamesModule>(config, *games, *users,
                                               *sessions, service.get());
        files = std::make_unique<AttachmentModule>(config, *attachments,
                                                   *sessions, *users);
        module->registerRoutes(server);
        files->registerRoutes(server);
        UserInfo identity;
        identity.name = "one";
        ON_CALL(auth, getUser(testing::_))
            .WillByDefault(testing::Return(identity));
        auto port = server.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        thread = std::thread([this] { server.listen_after_bind(); });
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_read_timeout(10);
        gallery = "/games/one/" + std::to_string(game_id) + "/screenshots";
    }
    void TearDown() override
    {
        server.stop();
        if(thread.joinable())
        {
            thread.join();
        }
    }
    httplib::Headers owner(bool json = true)
    {
        httplib::Headers headers{{"Cookie", "access-token=valid"},
                                 {"Origin", "http://localhost"}};
        if(json)
        {
            headers.emplace("Accept", "application/json");
        }
        return headers;
    }
    std::string multipart(const std::string& field, const std::string& name,
                          const std::string& bytes)
    {
        return "--shot\r\nContent-Disposition: form-data; name=\"" + field +
               "\"; filename=\"" + name +
               "\"\r\n"
               "Content-Type: image/png\r\n\r\n" +
               bytes + "\r\n";
    }
    httplib::Result post(const httplib::Headers& headers,
                         const std::string& body)
    {
        return client->Post(gallery + "/upload", headers, body,
                            "multipart/form-data; boundary=shot");
    }
};

TEST_F(ScreenshotHTTP, PublicGalleryOwnerUploadDuplicatesAndDeletion)
{
    auto empty = client->Get(gallery);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, 200);
    EXPECT_THAT(empty->body, testing::HasSubstr("No screenshots yet"));
    EXPECT_THAT(empty->body,
                testing::Not(testing::HasSubstr("id=\"ScreenshotUpload\"")));
    auto body = multipart("file", "moon.png", image()) + "--shot--\r\n";
    auto saved = post(owner(), body);
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->status, 200) << saved->body;
    auto json = nlohmann::json::parse(saved->body);
    EXPECT_FALSE(json["already_uploaded"]);
    auto original = json["original_url"].get<std::string>();
    auto preview = json["preview"]["url"].get<std::string>();
    EXPECT_NE(original, preview);
    auto duplicate = post(owner(), body);
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(nlohmann::json::parse(duplicate->body)["already_uploaded"]);
    EXPECT_EQ(count("GameScreenshots"), 1);
    auto page = client->Get(gallery);
    ASSERT_TRUE(page);
    EXPECT_THAT(page->body, testing::HasSubstr("href=\"" + original + "\""));
    EXPECT_THAT(page->body,
                testing::Not(testing::HasSubstr("src=\"" + original + "\"")));
    EXPECT_EQ(page->get_header_value("Cache-Control"), "no-store");
    auto compressed = client->Get(preview);
    ASSERT_TRUE(compressed);
    EXPECT_EQ(compressed->get_header_value("Content-Type"), "image/webp");
    EXPECT_EQ(compressed->get_header_value("X-Content-Type-Options"),
              "nosniff");
    auto deletion =
        gallery + "/" + std::to_string(json["id"].get<int64_t>()) + "/delete";
    auto denied = client->Get(deletion);
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 401);
    auto confirmation = client->Get(deletion, owner());
    ASSERT_TRUE(confirmation);
    EXPECT_EQ(confirmation->status, 200);
    EXPECT_THAT(confirmation->body, testing::HasSubstr("Delete screenshot?"));
    auto removed = client->Post(deletion, owner(), "",
                                "application/x-www-form-urlencoded");
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->status, 303);
    EXPECT_EQ(count("Attachments"), 0);
    EXPECT_EQ(count("GameScreenshots"), 0);
}

TEST_F(ScreenshotHTTP, AuthorizationOriginFieldConventionsAndMalformedMultipart)
{
    auto body = multipart("file", "moon.png", image()) + "--shot--\r\n";
    auto guest = post({{"Accept", "application/json"}}, body);
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest->status, 401);
    auto headers = owner();
    headers.erase("Origin");
    auto origin = post(headers, body);
    ASSERT_TRUE(origin);
    EXPECT_EQ(origin->status, 403);
    UserInfo foreign;
    foreign.name = "two";
    ON_CALL(auth, getUser(testing::_)).WillByDefault(testing::Return(foreign));
    auto nonowner = post(owner(), body);
    ASSERT_TRUE(nonowner);
    EXPECT_EQ(nonowner->status, 403);
    foreign.name = "one";
    ON_CALL(auth, getUser(testing::_)).WillByDefault(testing::Return(foreign));
    for(auto malformed :
        {multipart("files", "moon.png", image()) + "--shot--\r\n",
         multipart("file", "", image()) + "--shot--\r\n",
         multipart("file", "first.png", image()) +
             multipart("files", "second.png", image()) + "--shot--\r\n",
         multipart("file", "first.png", image()) +
             multipart("file", "second.png", image()) + "--shot--\r\n",
         multipart("file", "moon.png", image())})
    {
        auto rejected = post(owner(), malformed);
        ASSERT_TRUE(rejected);
        EXPECT_EQ(rejected->status, 400) << rejected->body;
        EXPECT_EQ(count("Attachments"), 0);
    }
    auto wrong_media =
        client->Post(gallery + "/upload", owner(), "{}", "application/json");
    ASSERT_TRUE(wrong_media);
    EXPECT_EQ(wrong_media->status, 415);
}

TEST_F(ScreenshotHTTP, NativeBatchKeepsEarlierSuccessAndRendersSafeResults)
{
    auto body = multipart("files", "good & mountain.png", image()) +
                multipart("files", "broken.png", "invalid image") +
                "--shot--\r\n";
    auto result = post(owner(false), body);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200) << result->body;
    EXPECT_THAT(result->body, testing::HasSubstr("good &amp; mountain.png"));
    EXPECT_THAT(result->body, testing::HasSubstr("broken.png"));
    EXPECT_THAT(result->body, testing::HasSubstr("Uploaded"));
    EXPECT_EQ(result->get_header_value("Cache-Control"), "no-store");
    EXPECT_EQ(count("GameScreenshots"), 1);
    EXPECT_EQ(count("Attachments"), 1);
    body = multipart("files", "complete.png", image()) +
           multipart("files", "interrupted.png", image());
    auto partial = post(owner(false), body);
    ASSERT_TRUE(partial);
    EXPECT_THAT(partial->body, testing::HasSubstr("interrupted.png"));
    EXPECT_EQ(count("GameScreenshots"), 2);
    EXPECT_EQ(count("Attachments"), 2);
}

TEST_F(Screenshots, AttachmentDeletionDuringConversionCannotResurrectRecords)
{
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(1, "deleted.png"));
    ASSERT_TRUE(writer.append(image()));
    ASSIGN_OR_FAIL(auto upload, writer.finish());
    ASSERT_TRUE(attachments->ensureImageVariant(
        1, upload.record.id, screenshotOptions(config.screenshot_thumbnail)));
    ASSERT_TRUE(attachments->ensureImageVariant(
        1, upload.record.id, screenshotOptions(config.screenshot_preview)));
    ASSERT_TRUE(attachments->deleteAttachment(1, upload.record.id));
    EXPECT_FALSE(attachments->writeMetadata(
        [&](UploadDatabase& db) -> E<void>
        {
            DO_OR_RETURN(associateScreenshot(db, config, 1, game_id,
                                             upload.record, false));
            return {};
        }));
    EXPECT_FALSE(attachments->ensureImageVariant(
        1, upload.record.id, screenshotOptions(config.screenshot_preview)));
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("Attachments"), 0);
    EXPECT_EQ(count("AttachmentVariants"), 0);
}

TEST_F(Screenshots, ExistingAttachmentCanBeReusedAndDeletedWithScreenshot)
{
    auto bytes = image();
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(1, "existing.png"));
    ASSERT_TRUE(writer.append(bytes));
    ASSIGN_OR_FAIL(auto upload, writer.finish());
    ASSIGN_OR_FAIL(auto shot, this->upload(game_id, "existing.png", bytes));
    EXPECT_EQ(shot.attachment.id, upload.record.id);
    EXPECT_FALSE(shot.already_uploaded);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    ASSERT_TRUE(service->remove(1, game_id, shot.id));
    EXPECT_FALSE(attachments->getAttachment(1, upload.record.id));
}

TEST_P(ScreenshotCodecs, RetainsSupportedMetadataAndAppliesOrientationOnce)
{
    auto settings = config;
    settings.screenshot_thumbnail.codec = GetParam();
    settings.screenshot_preview.codec = GetParam();
    auto available = validateScreenshotEncoders(settings);
    if(!available)
    {
        if(GetParam() == ScreenshotCodec::WEBP)
        {
            FAIL();
        }
        GTEST_SKIP() << errorMsg(available.error());
    }
    Magick::Image pixels(Magick::Geometry(20, 10), Magick::Color("red"));
    const unsigned char exif[] = {
        'E',  'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
        0x12, 1,   3,   0,   1, 0, 0,   0,   6,  0, 0, 0, 0, 0, 0, 0};
    pixels.profile("exif", Magick::Blob(exif, sizeof(exif)));
    pixels.orientation(Magick::RightTopOrientation);
    pixels.comment("Screenshot metadata");
    std::string xmp = "<x:xmpmeta xmlns:x='adobe:ns:meta/'/>";
    pixels.profile("xmp", Magick::Blob(xmp.data(), xmp.size()));
    Magick::Blob blob;
    pixels.write(&blob, "JPEG");
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(1, "orientation.jpg"));
    ASSERT_TRUE(
        writer.append({static_cast<const char*>(blob.data()), blob.length()}));
    ASSIGN_OR_FAIL(auto upload, writer.finish());
    ASSIGN_OR_FAIL(auto variant, attachments->ensureImageVariant(
                                     1, upload.record.id,
                                     {8, 8, 90, screenshotFormat(GetParam())}));
    EXPECT_EQ(variant.width, 4);
    EXPECT_EQ(variant.height, 8);
    Magick::Image decoded(
        attachmentPath(attachments->directory(), variant.file.hash)->string());
    EXPECT_GT(decoded.profile("exif").length(), 0);
    EXPECT_GT(decoded.profile("xmp").length(), 0);
    EXPECT_TRUE(decoded.orientation() == Magick::TopLeftOrientation ||
                decoded.orientation() == Magick::UndefinedOrientation);
    decoded.autoOrient();
    EXPECT_EQ(decoded.columns(), 4);
    EXPECT_EQ(decoded.rows(), 8);
    if(GetParam() == ScreenshotCodec::JPEG)
    {
        EXPECT_EQ(decoded.comment(), "Screenshot metadata");
    }
}

TEST_F(Screenshots,
       RejectsInvalidProfilesAndPreservesTaggedAndUntaggedOriginals)
{
    auto root =
        std::filesystem::path(NSWEEKLY_SOURCE_DIR) / "tests/fixtures/color";
    auto invalid = readBytes(root / "invalid_profile.png");
    EXPECT_FALSE(upload(game_id, "invalid.png", invalid));
    EXPECT_EQ(count("Attachments"), 0);
    for(auto name : {"unprofiled.png", "tagged.png"})
    {
        auto bytes = readBytes(root / name);
        ASSIGN_OR_FAIL(auto shot, upload(game_id, name, bytes));
        EXPECT_EQ(shot.thumbnail.width, 1);
        EXPECT_EQ(shot.preview.height, 1);
        EXPECT_EQ(readBytes(*attachmentPath(attachments->directory(),
                                            shot.attachment.file.hash)),
                  bytes);
    }
}

TEST_F(Screenshots, QueuedDuplicateConflictsUntilStorageCleanupCanRetry)
{
    auto bytes = image();
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "pending.png", bytes));
    ASSERT_TRUE(inspection->execute(R"(
CREATE TRIGGER FailRelease BEFORE DELETE ON Attachments
 BEGIN SELECT RAISE(ABORT,'injected release failure'); END)"));
    ASSERT_TRUE(service->remove(1, game_id, shot.id));
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("GameScreenshotCleanup"), 1);
    auto conflict = upload(game_id, "pending.png", bytes);
    ASSERT_FALSE(conflict);
    ASSERT_NE(conflict.error().as<HTTPError>(), nullptr);
    EXPECT_EQ(conflict.error().as<HTTPError>()->code, 409);
    EXPECT_EQ(count("Attachments"), 1);
    ASSERT_TRUE(inspection->execute("DROP TRIGGER FailRelease"));
    ASSERT_TRUE(service->cleanup());
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    EXPECT_EQ(count("Attachments"), 0);
}

class ScreenshotHTTPLimit : public ScreenshotHTTP
{
protected:
    void SetUp() override
    {
        config.attachment_upload_limit = 100;
        ScreenshotHTTP::SetUp();
    }
};

TEST_F(ScreenshotHTTPLimit, PerFileLimitsReturn413AndNativeBatchContinues)
{
    auto body =
        multipart("file", "large.png", std::string(101, 'a')) + "--shot--\r\n";
    auto result = post(owner(), body);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 413);
    EXPECT_EQ(nlohmann::json::parse(result->body)["error"], "upload_limit");
    EXPECT_EQ(count("Attachments"), 0);
    auto small = readBytes(std::filesystem::path(NSWEEKLY_SOURCE_DIR) /
                           "tests/fixtures/color/unprofiled.png");
    ASSERT_LT(small.size(), 100);
    body = multipart("files", "large.png", std::string(101, 'a')) +
           multipart("files", "small.png", small) + "--shot--\r\n";
    result = post(owner(false), body);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_THAT(result->body,
                testing::HasSubstr("File exceeds the upload limit"));
    EXPECT_THAT(result->body, testing::HasSubstr("Uploaded"));
    EXPECT_EQ(count("Attachments"), 1);
    EXPECT_EQ(count("GameScreenshots"), 1);
}

TEST_F(ScreenshotHTTP, PublicServingUsesActualAvifAndJxlMimeTypes)
{
    ASSIGN_OR_FAIL(auto shot, upload(game_id, "source.png", image()));
    for(auto codec : {ScreenshotCodec::AVIF, ScreenshotCodec::JXL})
    {
        auto settings = config;
        settings.screenshot_thumbnail.codec = codec;
        settings.screenshot_preview.codec = codec;
        auto available = validateScreenshotEncoders(settings);
        if(!available)
        {
            std::cout << "Optional codec unavailable: "
                      << screenshotFormat(codec) << '\n';
            continue;
        }
        auto format = screenshotFormat(codec);
        ASSIGN_OR_FAIL(auto variant,
                       attachments->ensureImageVariant(1, shot.attachment.id,
                                                       {8, 8, 85, format}));
        EXPECT_EQ(variant.file.mime_type, "image/" + format);
        auto response = client->Get(variant.public_url);
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
        EXPECT_EQ(response->get_header_value("Content-Type"),
                  "image/" + format);
        EXPECT_THAT(response->get_header_value("Content-Disposition"),
                    testing::StartsWith("inline"));
        EXPECT_EQ(response->get_header_value("X-Content-Type-Options"),
                  "nosniff");
        EXPECT_FALSE(response->body.empty());
    }
}

TEST_F(ScreenshotHTTP, InvalidUtf8FilenameReturnsDisplaySafeJson)
{
    auto body = multipart("file", std::string("bad\xff.png", 8), image()) +
                "--shot--\r\n";
    auto rejected = post(owner(), body);
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 422);
    auto result = nlohmann::json::parse(rejected->body);
    EXPECT_EQ(result["error"], "invalid_image");
    EXPECT_EQ(result["filename"], "Invalid filename");
    EXPECT_EQ(count("Attachments"), 0);
}

TEST_F(Screenshots,
       AssociationFailureRollsBackNewVariantsAndKeepsExistingOriginal)
{
    auto pixels = image();
    ASSERT_TRUE(inspection->execute(R"(
CREATE TRIGGER FailAssociation BEFORE INSERT ON GameScreenshots
 BEGIN SELECT RAISE(ABORT,'injected association failure'); END)"));
    EXPECT_FALSE(upload(game_id, "new.png", pixels));
    EXPECT_EQ(count("Attachments"), 0);
    EXPECT_EQ(count("AttachmentFiles"), 0);
    EXPECT_EQ(count("AttachmentVariants"), 0);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    ASSIGN_OR_FAIL(auto writer, attachments->beginUpload(1, "existing.png"));
    ASSERT_TRUE(writer.append(pixels));
    ASSIGN_OR_FAIL(auto kept, writer.finish());
    EXPECT_FALSE(upload(game_id, "existing.png", pixels));
    EXPECT_EQ(count("Attachments"), 1);
    EXPECT_EQ(count("AttachmentFiles"), 1);
    EXPECT_EQ(count("AttachmentVariants"), 0);
    EXPECT_EQ(count("GameScreenshots"), 0);
    EXPECT_EQ(count("GameScreenshotCleanup"), 0);
    EXPECT_EQ(readBytes(*attachmentPath(attachments->directory(),
                                        kept.record.file.hash)),
              pixels);
}
