#include <memory>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "app.hpp"
#include "auth_mock.hpp"
#include "storage_mock.hpp"
#include "route_urls.hpp"
#include "test_utils.hpp"

using ::testing::Return;
using ::testing::HasSubstr;

TEST(App, StartsAndStopsServerThread)
{
    Configuration config{};
    config.data_dir = NSWEEKLY_SOURCE_DIR;
    config.listen_address = "127.0.0.1";
    config.listen_port = 0;
    ASSIGN_OR_FAIL(auto app, App::create(config,
        std::make_unique<AuthMock>(), std::make_unique<UserDataMock>(),
        std::make_unique<WeeklyDataMock>()));

    auto started = app->start();
    app->stop();
    app->wait();
    EXPECT_TRUE(started.has_value());
}

TEST(App, IndexCanRedirectWhenLoggedIn)
{
    Configuration config{};
    auto auth = std::make_unique<AuthMock>();
    Tokens expected_tokens;
    expected_tokens.access_token = "aaa";
    UserInfo expected_user;
    expected_user.name = "mw";

    EXPECT_CALL(*auth, getUser(expected_tokens)).WillOnce(Return(expected_user));
    ASSIGN_OR_FAIL(auto user_db, SQLite::connectMemory());
    auto users = std::make_unique<UserDataSqlite>(std::move(user_db));
    ASSERT_TRUE(users->initializeSchema());
    ASSIGN_OR_FAIL(auto weekly_db, SQLite::connectMemory());
    auto weeklies = std::make_unique<WeeklyDataSqlite>(std::move(weekly_db));
    ASSIGN_OR_FAIL(auto app, App::create(config, std::move(auth),
        std::move(users), std::move(weeklies)));

    httplib::Request http_req;
    http_req.set_header("Cookie", "access-token=aaa");
    httplib::Response res;
    app->handleIndex(http_req, res);
    EXPECT_EQ(res.status, 302);
    EXPECT_EQ(res.get_header_value("Location"), urlFor("weekly", "mw"));
}

TEST(App, IndexCanRedirectWhenNotLoggedIn)
{
    Configuration config{};
    config.guest_index = GuestIndex::USER_WEEKLY;
    config.guest_index_user = "mw";

    auto auth = std::make_unique<AuthMock>();
    ASSIGN_OR_FAIL(auto user_db, SQLite::connectMemory());
    auto users = std::make_unique<UserDataSqlite>(std::move(user_db));
    ASSERT_TRUE(users->initializeSchema());
    ASSIGN_OR_FAIL(auto weekly_db, SQLite::connectMemory());
    auto weeklies = std::make_unique<WeeklyDataSqlite>(std::move(weekly_db));
    ASSIGN_OR_FAIL(auto app, App::create(config, std::move(auth),
        std::move(users), std::move(weeklies)));

    httplib::Request http_req;
    httplib::Response res;
    app->handleIndex(http_req, res);
    EXPECT_EQ(res.status, 301);
    EXPECT_EQ(res.get_header_value("Location"), "/login");
    EXPECT_EQ(res.get_header_value("Location", "", 1), weeklyURL("mw"));
}

TEST(App, IndexCanRefreshToken)
{
    Configuration config{};
    config.guest_index = GuestIndex::USER_WEEKLY;
    config.guest_index_user = "mw";
    Tokens expected_tokens_after_refresh;
    expected_tokens_after_refresh.access_token = "aaa";
    UserInfo expected_user;
    expected_user.name = "mw";

    auto auth = std::make_unique<AuthMock>();
    EXPECT_CALL(*auth, getUser(expected_tokens_after_refresh))
        .WillOnce(Return(expected_user));
    EXPECT_CALL(*auth, refreshTokens("bbb"))
        .WillOnce(Return(expected_tokens_after_refresh));

    ASSIGN_OR_FAIL(auto user_db, SQLite::connectMemory());
    auto users = std::make_unique<UserDataSqlite>(std::move(user_db));
    ASSERT_TRUE(users->initializeSchema());
    ASSIGN_OR_FAIL(auto weekly_db, SQLite::connectMemory());
    auto weeklies = std::make_unique<WeeklyDataSqlite>(std::move(weekly_db));
    ASSIGN_OR_FAIL(auto app, App::create(config, std::move(auth),
        std::move(users), std::move(weeklies)));

    httplib::Request req;
    req.set_header("Cookie", "refresh-token=bbb");
    httplib::Response res;
    app->handleIndex(req, res);
    EXPECT_EQ(res.status, 302);
    EXPECT_THAT(res.get_header_value("Set-Cookie"),
                HasSubstr("access-token=aaa"));
    EXPECT_EQ(res.get_header_value("Location"), urlFor("weekly", "mw"));
}


using ::testing::_;

TEST(App, InjectionRequiresAllDependencies)
{
    Configuration config{};
    auto app = App::create(config, nullptr,
        std::make_unique<UserDataMock>(), std::make_unique<WeeklyDataMock>());
    ASSERT_FALSE(app);
    EXPECT_EQ(errorMsg(app.error()), "Missing application dependency");
}

TEST(App, FailedRefreshFallsBackToGuestLookup)
{
    Configuration config{};
    config.guest_index_user = "guest";
    auto auth = std::make_unique<AuthMock>();
    EXPECT_CALL(*auth, refreshTokens("expired")).WillOnce(Return(
        std::unexpected(httpError(401, "Expired"))));
    auto users = std::make_unique<UserDataMock>();
    EXPECT_CALL(*users, getUserID("guest"))
        .WillOnce(Return(std::optional<int64_t>{7}));
    ASSIGN_OR_FAIL(auto app, App::create(config, std::move(auth),
        std::move(users), std::make_unique<WeeklyDataMock>()));
    httplib::Request req;
    req.set_header("Cookie", "refresh-token=expired");
    httplib::Response res;
    app->handleIndex(req, res);
    EXPECT_EQ(res.status, 301);
    EXPECT_EQ(res.get_header_value("Location"), "/weekly/guest");
    EXPECT_FALSE(res.has_header("Set-Cookie"));
}

TEST(App, InvalidPrefixFailsBeforeOpeningDependencies)
{
    Configuration config{};
    config.url_prefix = "not a URL";
    int exit_code = 0;
    EXPECT_FALSE(App::create(config, exit_code));
    EXPECT_EQ(exit_code, 4);
}
