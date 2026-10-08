#include <fstream>
#include <regex>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "weekly_module.hpp"
#include "auth_mock.hpp"
#include "storage_mock.hpp"
#include "user_sql.hpp"
#include "test_utils.hpp"

using ::testing::Return;
using ::testing::HasSubstr;

namespace
{
std::string normalizeHtml(const std::string& html)
{
    return std::regex_replace(html, std::regex(
        R"(20[0-9]{2}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2})"),
        "UPDATE_TIME");
}
} // namespace

TEST(WeeklyModule, WeeklyPagesRetainContentAndPreviewUrls)
{
    Configuration config{};
    config.data_dir = NSWEEKLY_SOURCE_DIR;
    auto auth = std::make_unique<AuthMock>();
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    ASSERT_TRUE(initializeUsers(*db));
    auto data = std::make_unique<WeeklyDataSqlite>(std::move(db));
    ASSERT_TRUE(data->initializeSchema());
    WeeklyPost post;
    post.week_begin = *strToDate("2000-01-03");
    post.raw_content = "**fixture**";
    post.format = WeeklyPost::MARKDOWN;
    ASSERT_TRUE(data->updateWeekly("mw", std::move(post)));
    SessionService sessions(*auth);
    WeeklyModule module(config, *data, sessions);
    mw::HTTPServer::Request req;
    req.target = "/weekly/mw/2000-01-03";
    mw::HTTPServer::Response res;
    module.handleUserWeekly(req, res, "mw", *strToDate("2000-01-03"));
    std::ifstream fixture(std::string(NSWEEKLY_SOURCE_DIR) +
                          "/tests/fixtures/weekly.html");
    std::string baseline(std::istreambuf_iterator<char>{fixture}, {});
    // The update time is the only clock-dependent field in this fixture.
    EXPECT_EQ(normalizeHtml(res.body), normalizeHtml(baseline));
    EXPECT_THAT(res.body, HasSubstr("<strong>fixture</strong>"));
    EXPECT_THAT(res.body, HasSubstr("/statics/style.css"));
    EXPECT_THAT(res.body, HasSubstr("2000-01-03"));
    EXPECT_THAT(res.get_header_value("Content-Type"), HasSubstr("text/html"));
}

TEST(WeeklyModule, EditRejectsGuestsAndNonMondays)
{
    Configuration config{};
    auto auth = std::make_unique<AuthMock>();
    Tokens tokens;
    tokens.access_token = "aaa";
    EXPECT_CALL(*auth, getUser(tokens)).WillOnce(Return(UserInfo{"", "mw"}));
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    ASSERT_TRUE(initializeUsers(*db));
    auto data = std::make_unique<WeeklyDataSqlite>(std::move(db));
    ASSERT_TRUE(data->initializeSchema());
    SessionService sessions(*auth);
    WeeklyModule module(config, *data, sessions);
    mw::HTTPServer::Request req;
    mw::HTTPServer::Response guest;
    module.handleEdit(req, guest, "mw", *strToDate("2000-01-03"));
    EXPECT_EQ(guest.status, 401);
    req.set_header("Cookie", "access-token=aaa");
    mw::HTTPServer::Response tuesday;
    module.handleEdit(req, tuesday, "mw", *strToDate("2000-01-04"));
    EXPECT_EQ(tuesday.status, 404);
}


using ::testing::_;

TEST(WeeklyModule, EmptyEditResultReturnsNotFound)
{
    Configuration config{};
    AuthMock auth;
    WeeklyDataMock data;
    EXPECT_CALL(auth, getUser(_)).WillOnce(Return(UserInfo{"", "mw"}));
    EXPECT_CALL(data, getWeeklies("mw", _, _))
        .WillOnce(Return(std::vector<WeeklyPost>{}));
    SessionService sessions(auth);
    WeeklyModule module(config, data, sessions);
    mw::HTTPServer::Request req;
    req.set_header("Cookie", "access-token=aaa");
    mw::HTTPServer::Response res;
    module.handleEditFrontEnd(req, res, "mw", *strToDate("2000-01-03"));
    EXPECT_EQ(res.status, 404);
}

TEST(WeeklyModule, StorageErrorsMapToExistingResponses)
{
    Configuration config{};
    AuthMock auth;
    WeeklyDataMock data;
    EXPECT_CALL(data, getWeeklies("mw", _, _)).WillOnce(Return(
        std::unexpected(httpError(404, "Missing"))));
    EXPECT_CALL(data, getWeeklies("broken", _, _)).WillOnce(Return(
        std::unexpected(runtimeError("Database failed"))));
    SessionService sessions(auth);
    WeeklyModule module(config, data, sessions);
    mw::HTTPServer::Request req;
    mw::HTTPServer::Response missing;
    module.handleUserWeekly(req, missing, "mw", *strToDate("2000-01-03"));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.body, "Missing");
    mw::HTTPServer::Response broken;
    module.handleUserWeeklies(req, broken, "broken");
    EXPECT_EQ(broken.status, 500);
    EXPECT_EQ(broken.body, "Database failed");
}

TEST(WeeklyModule, RefreshedSessionDoesNotSetCookies)
{
    Configuration config{};
    AuthMock auth;
    WeeklyDataMock data;
    Tokens tokens;
    tokens.access_token = "new";
    EXPECT_CALL(auth, refreshTokens("refresh")).WillOnce(Return(tokens));
    EXPECT_CALL(auth, getUser(tokens)).WillOnce(Return(UserInfo{"", "mw"}));
    EXPECT_CALL(data, getWeeklies("mw", _, _))
        .WillOnce(Return(std::vector<WeeklyPost>{}));
    SessionService sessions(auth);
    WeeklyModule module(config, data, sessions);
    mw::HTTPServer::Request req;
    req.set_header("Cookie", "refresh-token=refresh");
    mw::HTTPServer::Response res;
    module.handleUserWeekly(req, res, "mw", *strToDate("2000-01-03"));
    EXPECT_EQ(res.status, 404);
    EXPECT_FALSE(res.has_header("Set-Cookie"));
}

TEST(WeeklyModule, EditRequiresMatchingOwnerAndPropagatesSaveFailure)
{
    Configuration config{};
    AuthMock auth;
    WeeklyDataMock data;
    EXPECT_CALL(auth, getUser(_)).WillRepeatedly(Return(UserInfo{"", "mw"}));
    EXPECT_CALL(data, updateWeekly("mw", _)).WillOnce(Return(
        std::unexpected(runtimeError("Save failed"))));
    SessionService sessions(auth);
    WeeklyModule module(config, data, sessions);
    mw::HTTPServer::Request req;
    req.set_header("Cookie", "access-token=aaa");
    mw::HTTPServer::Response forbidden;
    module.handleEdit(req, forbidden, "other", *strToDate("2000-01-03"));
    EXPECT_EQ(forbidden.status, 401);
    mw::HTTPServer::Response failed;
    module.handleEdit(req, failed, "mw", *strToDate("2000-01-03"));
    EXPECT_EQ(failed.status, 500);
    EXPECT_EQ(failed.body, "Save failed");
}
