#include <gtest/gtest.h>
#include "session_service.hpp"
#include "auth_mock.hpp"
#include "test_utils.hpp"

using ::testing::Return;
using ::testing::_;

TEST(SessionService, MissingAndInvalidAccessTokensAreInvalid)
{
    AuthMock auth;
    SessionService sessions(auth);
    mw::HTTPServer::Request req;
    ASSIGN_OR_FAIL(auto missing, sessions.validateSession(req));
    EXPECT_EQ(missing.status, SessionValidation::INVALID);
    EXPECT_CALL(auth, getUser(_)).WillOnce(Return(
        std::unexpected(runtimeError("Expired"))));
    req.set_header("Cookie", "ignored; access-token=old; unrelated=value");
    ASSIGN_OR_FAIL(auto invalid, sessions.validateSession(req));
    EXPECT_EQ(invalid.status, SessionValidation::INVALID);
}

TEST(SessionService, ValidAccessSkipsRefresh)
{
    AuthMock auth;
    Tokens tokens;
    tokens.access_token = "good";
    EXPECT_CALL(auth, getUser(tokens)).WillOnce(Return(UserInfo{"id", "mw"}));
    EXPECT_CALL(auth, refreshTokens(_)).Times(0);
    SessionService sessions(auth);
    mw::HTTPServer::Request req;
    req.set_header("Cookie", "access-token=good; refresh-token=unused");
    ASSIGN_OR_FAIL(auto valid, sessions.validateSession(req));
    EXPECT_EQ(valid.status, SessionValidation::VALID);
    EXPECT_EQ(valid.user.name, "mw");
}

TEST(SessionService, ExpiredAccessFallsBackToRefresh)
{
    AuthMock auth;
    Tokens old_tokens;
    old_tokens.access_token = "old";
    Tokens new_tokens;
    new_tokens.access_token = "new";
    EXPECT_CALL(auth, getUser(old_tokens)).WillOnce(Return(
        std::unexpected(httpError(401, "Expired"))));
    EXPECT_CALL(auth, refreshTokens("refresh")).WillOnce(Return(new_tokens));
    EXPECT_CALL(auth, getUser(new_tokens))
        .WillOnce(Return(UserInfo{"id", "mw"}));
    SessionService sessions(auth);
    mw::HTTPServer::Request req;
    req.set_header("Cookie", "access-token=old; refresh-token=refresh");
    ASSIGN_OR_FAIL(auto refreshed, sessions.validateSession(req));
    EXPECT_EQ(refreshed.status, SessionValidation::REFRESHED);
    EXPECT_EQ(refreshed.new_tokens, new_tokens);
}

TEST(SessionService, RefreshAndIdentityFailuresPropagate)
{
    AuthMock auth;
    Tokens tokens;
    tokens.access_token = "new";
    EXPECT_CALL(auth, refreshTokens("bad")).WillOnce(Return(
        std::unexpected(httpError(401, "Expired"))));
    EXPECT_CALL(auth, refreshTokens("good")).WillOnce(Return(tokens));
    EXPECT_CALL(auth, getUser(tokens)).WillOnce(Return(
        std::unexpected(runtimeError("Identity failed"))));
    SessionService sessions(auth);
    mw::HTTPServer::Request bad;
    bad.set_header("Cookie", "refresh-token=bad");
    auto failed = sessions.validateSession(bad);
    ASSERT_FALSE(failed);
    const auto* error = failed.error().as<HTTPError>();
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(*error, (HTTPError{401, "Expired"}));
    mw::HTTPServer::Request good;
    good.set_header("Cookie", "refresh-token=good");
    failed = sessions.validateSession(good);
    ASSERT_FALSE(failed);
    EXPECT_EQ(errorMsg(failed.error()), "Identity failed");
}

TEST(SessionService, CookieFormattingRetainsEncodingAndDefaultAges)
{
    AuthMock auth;
    SessionService sessions(auth);
    Tokens tokens;
    tokens.access_token = "a b";
    tokens.refresh_token = "x+y";
    mw::HTTPServer::Response res;
    sessions.setTokenCookies(tokens, res);
    EXPECT_EQ(res.get_header_value("Set-Cookie"),
              "access-token=a%20b; Max-Age=300");
    EXPECT_EQ(res.get_header_value("Set-Cookie", "", 1),
              "refresh-token=x%2By; Max-Age=1800");
    EXPECT_EQ(res.get_header_value_count("Set-Cookie"), 2);
}
