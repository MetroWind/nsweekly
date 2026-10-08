#include <thread>
#include <fstream>
#include <gtest/gtest.h>
#include "app.hpp"
#include "auth_mock.hpp"
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
        auto auth_owner = std::make_unique<AuthMock>();
        auth = auth_owner.get();
        auto user_owner = std::make_unique<UserDataMock>();
        users = user_owner.get();
        auto weekly_owner = std::make_unique<WeeklyDataMock>();
        weeklies = weekly_owner.get();
        ASSIGN_OR_FAIL(app, App::create(config, std::move(auth_owner),
            std::move(user_owner), std::move(weekly_owner)));
        app->registerRoutes(server);
        int port = server.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        listener = std::thread(&Routes::listen, this);
        server.wait_until_ready();
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_read_timeout(5);
    }

    void TearDown() override
    {
        server.stop();
        if(listener.joinable())
        {
            listener.join();
        }
    }

    void listen()
    {
        server.listen_after_bind();
    }

    std::unique_ptr<App> app;
    AuthMock* auth = nullptr;
    UserDataMock* users = nullptr;
    WeeklyDataMock* weeklies = nullptr;
    httplib::Server server;
    std::thread listener;
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
    EXPECT_THAT(edit->body, HasSubstr("/statics/preview.js"));
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
    auto preview = client->Get("/statics/preview.js");
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->status, 200);
    std::ifstream source(std::string(NSWEEKLY_SOURCE_DIR) +
                         "/statics/preview.js");
    EXPECT_EQ(preview->body,
              (std::string(std::istreambuf_iterator<char>{source}, {})));
    auto css = client->Get("/statics/style.css");
    ASSERT_TRUE(css);
    EXPECT_EQ(css->status, 200);
    EXPECT_THAT(css->get_header_value("Content-Type"), HasSubstr("text/css"));
}
