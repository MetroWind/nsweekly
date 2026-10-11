#include <fstream>
#include <gtest/gtest.h>
#include "app.hpp"
#include "attachment/attachment_test_support.hpp"
#include "auth/auth_mock.hpp"
#include "storage_mock.hpp"
#include "test_utils.hpp"

using ::testing::_;
using ::testing::Return;
using ::testing::HasSubstr;

namespace
{
// Runs the composed routes on loopback and stops before App is destroyed.
class Routes : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Configuration config{};
        config.data_dir = NSWEEKLY_SOURCE_DIR;
        config.url_prefix = "http://tracker.example";
        config.listen_address = "127.0.0.1";
        httplib::Server port_probe;
        config.listen_port = port_probe.bind_to_any_port(config.listen_address);
        port_probe.stop();
        ASSERT_GT(config.listen_port, 0);
        auto auth_owner = std::make_unique<AuthMock>();
        auth = auth_owner.get();
        auto user_owner = std::make_unique<UserDataMock>();
        users = user_owner.get();
        auto game_owner = std::make_unique<GameDataMock>();
        games = game_owner.get();
        auto weekly_owner = std::make_unique<WeeklyDataMock>();
        weeklies = weekly_owner.get();
        ASSIGN_OR_FAIL(auto attachment_owner,
            testAttachmentService(config,attachment_root));
        attachments = attachment_owner.get();
        ASSIGN_OR_FAIL(app, App::create(config, std::move(auth_owner),
            std::move(user_owner), std::move(weekly_owner), std::move(game_owner),
            std::move(attachment_owner)));
        ASSERT_TRUE(app->start().has_value());
        server_started = true;
        client = std::make_unique<httplib::Client>(
            config.listen_address, config.listen_port);
        client->set_read_timeout(5);
    }

    void TearDown() override
    {
        if(server_started)
        {
            app->stop();
            app->wait();
        }
    }

    TemporaryDatabase attachment_root;
    AttachmentService* attachments = nullptr;
    std::unique_ptr<App> app;
    AuthMock* auth = nullptr;
    UserDataMock* users = nullptr;
    WeeklyDataMock* weeklies = nullptr;
    GameDataMock* games = nullptr;
    bool server_started = false;
    std::unique_ptr<httplib::Client> client;
};

WeeklyPost fixturePost()
{
    WeeklyPost post{};
    post.format = WeeklyPost::MARKDOWN;
    post.week_begin = *strToDate("2000-01-03");
    post.update_time = *strToDate("2000-01-04");
    post.raw_content = "**fixture**";
    post.author = "mw";
    post.language = "en-US";
    return post;
}
} // namespace

TEST_F(Routes, LoginCallbackAndRootRetainRedirectsAndCookies)
{
    EXPECT_CALL(*auth, initialURL()).WillOnce(Return("https://provider/login"));
    auto login = client->Get("/login");
    ASSERT_TRUE(login);
    EXPECT_EQ(login->status, 301);
    EXPECT_EQ(login->get_header_value("Location"), "https://provider/login");

    Tokens tokens;
    tokens.access_token = "access";
    tokens.refresh_token = "refresh";
    EXPECT_CALL(*auth, authenticate("code")).WillOnce(Return(tokens));
    EXPECT_CALL(*auth, getUser(tokens)).WillOnce(Return(UserInfo{"", "mw"}));
    auto callback = client->Get("/openid-redirect?code=code");
    ASSERT_TRUE(callback);
    EXPECT_EQ(callback->status, 301);
    EXPECT_EQ(callback->get_header_value("Location"), "/");
    EXPECT_EQ(callback->get_header_value_count("Set-Cookie"), 2);

    EXPECT_CALL(*auth, refreshTokens("refresh")).WillOnce(Return(tokens));
    EXPECT_CALL(*auth, getUser(tokens)).WillOnce(Return(UserInfo{"", "mw"}));
    httplib::Headers headers{{"Cookie", "refresh-token=refresh"}};
    auto root = client->Get("/", headers);
    ASSERT_TRUE(root);
    EXPECT_EQ(root->status, 302);
    EXPECT_EQ(root->get_header_value("Location"), "/weekly/mw");
    EXPECT_EQ(root->get_header_value_count("Set-Cookie"), 2);
}

TEST_F(Routes, WeeklyPathsRetainHtmlAndNewestFirstOrder)
{
    auto post = fixturePost();
    auto newer = post;
    newer.week_begin += std::chrono::days(7);
    newer.raw_content = "newer fixture";
    EXPECT_CALL(*weeklies, getWeeklies("mw", _, _))
        .WillOnce(Return(std::vector<WeeklyPost>{post, newer}))
        .WillOnce(Return(std::vector<WeeklyPost>{post}));
    auto list = client->Get("/weekly/mw");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    EXPECT_THAT(list->get_header_value("Content-Type"), HasSubstr("text/html"));
    EXPECT_LT(list->body.find("newer fixture"),
              list->body.find("<strong>fixture</strong>"));
    auto single = client->Get("/weekly/mw/2000-01-03");
    ASSERT_TRUE(single);
    EXPECT_EQ(single->status, 200);
    EXPECT_THAT(single->body, HasSubstr("<strong>fixture</strong>"));
    EXPECT_THAT(single->body, HasSubstr("/weekly/mw/2000-01-03"));
    auto wrong_verb = client->Post("/weekly/mw", "", "text/plain");
    ASSERT_TRUE(wrong_verb);
    EXPECT_EQ(wrong_verb->status, 404);
}

TEST_F(Routes, InvalidDatesAndMissingWeekliesHaveDefinedErrors)
{
    for(const auto& date: {"bad", "2000-02-30"})
    {
        auto weekly = client->Get(std::string("/weekly/mw/") + date);
        ASSERT_TRUE(weekly);
        EXPECT_EQ(weekly->status, 400);
        EXPECT_EQ(weekly->body, "Invalid date");
        auto edit = client->Get(std::string("/edit/mw/") + date);
        ASSERT_TRUE(edit);
        EXPECT_EQ(edit->status, 400);
        auto save = client->Post(std::string("/edit/mw/") + date,
                                  "content=text", "application/x-www-form-urlencoded");
        ASSERT_TRUE(save);
        EXPECT_EQ(save->status, 400);
    }
    EXPECT_CALL(*weeklies, getWeeklies("mw", _, _))
        .WillOnce(Return(std::vector<WeeklyPost>{}));
    auto missing = client->Get("/weekly/mw/2000-01-04");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
}

TEST_F(Routes, EditPreviewAndSaveRetainRawContentAndAuthor)
{
    EXPECT_CALL(*auth, getUser(_))
        .WillRepeatedly(Return(UserInfo{"", "mw"}));
    EXPECT_CALL(*weeklies, getWeeklies("mw", _, _))
        .WillOnce(Return(std::vector<WeeklyPost>{fixturePost()}));
    httplib::Headers headers{{"Cookie", "access-token=access"}};
    auto edit = client->Get("/edit/mw/2000-01-03", headers);
    ASSERT_TRUE(edit);
    EXPECT_EQ(edit->status, 200);
    EXPECT_THAT(edit->body, HasSubstr("<textarea name=\"content\">**fixture**"));
    EXPECT_THAT(edit->body, HasSubstr("/statics/weekly/preview.js"));
    EXPECT_THAT(edit->body, HasSubstr("action=\"/edit/mw/2000-01-03\""));
    WeeklyPost saved{};
    EXPECT_CALL(*weeklies, updateWeekly("mw", _))
        .WillOnce([&saved](const std::string&, WeeklyPost&& post) -> E<void>
        {
            saved = std::move(post);
            return {};
        });
    auto save = client->Post("/edit/mw/2000-01-03", headers,
        "content=%23+Saved", "application/x-www-form-urlencoded");
    ASSERT_TRUE(save);
    EXPECT_EQ(save->status, 302);
    EXPECT_EQ(save->get_header_value("Location"), "/");
    EXPECT_EQ(saved.raw_content, "# Saved");
    EXPECT_EQ(saved.author, "mw");
    EXPECT_EQ(saved.week_begin, *strToDate("2000-01-03"));
    EXPECT_EQ(saved.format, WeeklyPost::MARKDOWN);
    EXPECT_EQ(saved.language, Configuration{}.default_lang);
}

TEST_F(Routes, EditRejectsGuestsOtherOwnersAndNonMondays)
{
    auto guest = client->Get("/edit/mw/2000-01-03");
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest->status, 401);
    EXPECT_CALL(*auth, getUser(_))
        .WillRepeatedly(Return(UserInfo{"", "mw"}));
    httplib::Headers headers{{"Cookie", "access-token=access"}};
    auto other = client->Get("/edit/other/2000-01-03", headers);
    ASSERT_TRUE(other);
    EXPECT_EQ(other->status, 401);
    auto tuesday = client->Post("/edit/mw/2000-01-04", headers,
        "content=text", "application/x-www-form-urlencoded");
    ASSERT_TRUE(tuesday);
    EXPECT_EQ(tuesday->status, 404);
}

TEST_F(Routes, StaticsServeUnchangedPreviewAsset)
{
    auto preview = client->Get("/statics/weekly/preview.js");
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->status, 200);
    std::ifstream source(std::string(NSWEEKLY_SOURCE_DIR) +
                         "/statics/weekly/preview.js");
    EXPECT_EQ(preview->body,
              (std::string(std::istreambuf_iterator<char>{source}, {})));
    auto css = client->Get("/statics/weekly/style.css");
    ASSERT_TRUE(css);
    EXPECT_EQ(css->status, 200);
    EXPECT_THAT(css->get_header_value("Content-Type"), HasSubstr("text/css"));
}

TEST_F(Routes, GamesRoutesPublicFormsAndEncodedUsername)
{
    EXPECT_CALL(*users, getUserID("alice/new"))
        .WillOnce(Return(std::optional<int64_t>{3}));
    EXPECT_CALL(*games, listGames("alice/new"))
        .WillOnce(Return(std::vector<GameRecord>{}));
    auto encoded = client->Get("/games/alice%2Fnew");
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded->status, 200);
    EXPECT_THAT(encoded->body, HasSubstr("alice/new’s Game Tracker"));
    auto canonical = client->Get("/games");
    ASSERT_TRUE(canonical);
    EXPECT_EQ(canonical->status, 308);
    EXPECT_EQ(canonical->get_header_value("Location"), "/games/");
    auto guest = client->Get("/games/alice/new");
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest->status, 401);
    EXPECT_CALL(*users, getUserID("alice"))
        .WillOnce(Return(std::optional<int64_t>{}));
    auto detail = client->Get("/games/alice/1");
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 404);
}

TEST_F(Routes, AttachmentRoutesUseInjectedStorageAndRefreshCookies)
{
    EXPECT_CALL(*auth,getUser(_)).WillRepeatedly(Return(UserInfo{"","mw"}));
    EXPECT_CALL(*users,ensureUser("mw")).WillRepeatedly(Return(E<void>{}));
    EXPECT_CALL(*users,getUserID("mw")).WillRepeatedly(
        Return(E<std::optional<int64_t>>{1}));
    ASSIGN_OR_FAIL(auto writer,attachments->beginUpload(1,"<file> café.txt"));
    ASSERT_TRUE(writer.append("abc"));
    ASSIGN_OR_FAIL(auto uploaded,writer.finish());
    auto guest = client->Get("/attachments/mw");
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest->status,401);
    httplib::Headers headers{{"Cookie","access-token=valid"}};
    auto manager = client->Get("/attachments/mw",headers);
    ASSERT_TRUE(manager);
    EXPECT_EQ(manager->status,200);
    EXPECT_THAT(manager->body,HasSubstr("&lt;file&gt; café.txt"));
    EXPECT_THAT(manager->body,HasSubstr("Copy URL"));
    EXPECT_EQ(manager->get_header_value("Cache-Control"),"no-store");
    auto foreign = client->Get("/attachments/another",headers);
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->status,403);
    auto public_file = client->Get(uploaded.public_url);
    ASSERT_TRUE(public_file);
    EXPECT_EQ(public_file->body,"abc");
    Tokens tokens;
    tokens.access_token = "access";
    tokens.refresh_token = "refresh";
    EXPECT_CALL(*auth,refreshTokens("refresh")).WillOnce(Return(tokens));
    httplib::Headers refresh{{"Cookie","refresh-token=refresh"}};
    auto refreshed = client->Get("/attachments/mw",refresh);
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(refreshed->status,200);
    EXPECT_EQ(refreshed->get_header_value_count("Set-Cookie"),2);
}

TEST_F(Routes, AttachmentJsonUploadRetainsNativeBehavior)
{
    EXPECT_CALL(*auth, getUser(_)).WillRepeatedly(Return(UserInfo{"", "mw"}));
    EXPECT_CALL(*users, ensureUser("mw")).WillRepeatedly(Return(E<void>{}));
    EXPECT_CALL(*users, getUserID("mw")).WillRepeatedly(
        Return(E<std::optional<int64_t>>{1}));
    httplib::Headers headers{{"Cookie", "access-token=valid"},
        {"Origin", "http://tracker.example"}, {"Accept", "application/json"}};
    httplib::UploadFormDataItems files{{"file", "abc", "café [x](y).txt",
        "application/octet-stream"}};
    auto created = client->Post("/attachments/mw/upload", headers, files);
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 200) << created->body;
    auto body = nlohmann::json::parse(created->body);
    EXPECT_EQ(body["filename"], "café [x](y).txt");
    EXPECT_EQ(body["byte_size"], 3);
    EXPECT_EQ(body["already_owned"], false);
    ASSIGN_OR_FAIL(auto records, attachments->listAttachments(1));
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(body["id"], records[0].id);
    EXPECT_EQ(body["mime_type"], records[0].file.mime_type);
    EXPECT_EQ(body["public_url"],
        *attachmentURL(records[0].file.hash, records[0].original_name));
    auto duplicate = client->Post("/attachments/mw/upload", headers, files);
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->status, 200);
    EXPECT_EQ(nlohmann::json::parse(duplicate->body)["already_owned"], true);
    headers.erase("Accept");
    auto native = client->Post("/attachments/mw/upload", headers, files);
    ASSERT_TRUE(native);
    EXPECT_EQ(native->status, 303);
    EXPECT_EQ(native->get_header_value("Location"),
        "/attachments/mw?status=duplicate");
    headers.emplace("Accept", "application/json");
    headers.erase("Origin");
    auto origin = client->Post("/attachments/mw/upload", headers, files);
    ASSERT_TRUE(origin);
    EXPECT_EQ(origin->status, 403);
    EXPECT_EQ(nlohmann::json::parse(origin->body)["error"]["code"],
        "origin_required");
    headers.erase("Cookie");
    auto guest = client->Post("/attachments/mw/upload", headers, files);
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest->status, 401);
    EXPECT_EQ(nlohmann::json::parse(guest->body)["error"]["code"],
        "login_required");
}

TEST_F(Routes, JournalDispatchAndStreamedRequestValidation)
{
    const std::string user = "alice/new";
    const std::string base = "/games/alice%2Fnew/7";
    GameInput input;
    input.name = "Game";
    GameRecord game{7, input};
    GameJournalRecord entry{42, 7, {"source"}, 100, 100};
    EXPECT_CALL(*auth, getUser(_)).WillRepeatedly(Return(UserInfo{"", user}));
    EXPECT_CALL(*users, ensureUser(user)).WillRepeatedly(Return(E<void>{}));
    EXPECT_CALL(*users, getUserID(user)).WillRepeatedly(
        Return(E<std::optional<int64_t>>{1}));
    EXPECT_CALL(*games, getGame(user, 7)).WillRepeatedly(
        Return(std::optional<GameRecord>{game}));
    EXPECT_CALL(*games, getJournalEntry(user, 7, 42)).WillRepeatedly(
        Return(std::optional<GameJournalRecord>{entry}));
    EXPECT_CALL(*games, createJournalEntry(user, 7, _)).WillOnce(Return(entry));
    EXPECT_CALL(*games, updateJournalEntry(user, 7, 42, _)).WillOnce(Return(entry));
    EXPECT_CALL(*games, deleteJournalEntry(user, 7, 42)).WillOnce(Return(true));
    httplib::Headers headers{{"Cookie", "access-token=valid"},
        {"Origin", "http://tracker.example"}};
    for(const auto *suffix : {"/journal/new", "/journal/42/edit",
        "/journal/42/delete"})
    {
        auto editor = client->Get(base + suffix, headers);
        ASSERT_TRUE(editor);
        EXPECT_EQ(editor->status, 200) << editor->body;
        auto saved = client->Post(base + suffix, headers,
            std::string(suffix).ends_with("delete") ? "" : "text=source",
            "application/x-www-form-urlencoded");
        ASSERT_TRUE(saved);
        EXPECT_EQ(saved->status, 303);
        EXPECT_EQ(saved->get_header_value("Location"), base +
            (std::string(suffix).ends_with("delete") ?
                "#JournalTitle" : "#journal-entry-42"));
    }
    for(const auto *suffix : {"/journal/42/edit/extra", "/journal/42",
        "/journal/new/extra", "/journal/42/view"})
    {
        auto bad = client->Get(base + suffix, headers);
        ASSERT_TRUE(bad);
        EXPECT_EQ(bad->status, 404);
    }
    for(const auto *suffix : {"/journal/0/edit", "/journal/-1/delete",
        "/journal/42x/edit", "/journal/9223372036854775808/edit"})
    {
        auto bad = client->Get(base + suffix, headers);
        ASSERT_TRUE(bad);
        EXPECT_EQ(bad->status, 400);
    }
    auto type = client->Post(base + "/journal/new", headers, "text=source",
        "text/plain");
    ASSERT_TRUE(type);
    EXPECT_EQ(type->status, 415);
    auto huge = client->Post(base + "/journal/new", headers,
        "text=" + std::string(1024 * 1024, 'a'),
        "application/x-www-form-urlencoded");
    ASSERT_TRUE(huge);
    EXPECT_EQ(huge->status, 413);
    Tokens tokens;
    tokens.access_token = "access";
    tokens.refresh_token = "refresh";
    EXPECT_CALL(*auth, refreshTokens("refresh")).WillOnce(Return(tokens));
    headers.erase("Cookie");
    headers.emplace("Cookie", "refresh-token=refresh");
    headers.erase("Origin");
    auto refreshed = client->Post(base + "/journal/new", headers, "text=source",
        "application/x-www-form-urlencoded");
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(refreshed->status, 403);
    EXPECT_EQ(refreshed->get_header_value_count("Set-Cookie"), 2);
}
