#include <memory>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "auth_module.hpp"
#include "auth_mock.hpp"
#include "route_urls.hpp"
#include "test_utils.hpp"

using ::testing::Return;

TEST(AuthModule, LoginBringsUserToLoginURL)
{
    auto auth = std::make_unique<AuthMock>();
    EXPECT_CALL(*auth, initialURL()).WillOnce(Return("http://aaa/"));
    SessionService sessions(*auth);
    AuthModule module(*auth, sessions);

    mw::HTTPServer::Response res;
    module.handleLogin(res);
    EXPECT_EQ(res.status, 301);
    EXPECT_EQ(res.get_header_value("Location"), "http://aaa/");
}

TEST(AuthModule, CanHandleOpenIDRedirect)
{
    auto auth = std::make_unique<AuthMock>();
    Tokens expected_tokens;
    expected_tokens.access_token = "bbb";
    UserInfo expected_user;
    expected_user.name = "mw";

    EXPECT_CALL(*auth, authenticate("aaa")).WillOnce(Return(expected_tokens));
    EXPECT_CALL(*auth, getUser(expected_tokens)).WillOnce(Return(expected_user));
    SessionService sessions(*auth);
    AuthModule module(*auth, sessions);

    mw::HTTPServer::Request req;
    req.params.emplace("code", "aaa");
    mw::HTTPServer::Response res;
    module.handleOpenIDRedirect(req, res);
    EXPECT_EQ(res.status, 301);
    EXPECT_EQ(res.get_header_value("Location"), urlFor("index", ""));
}

TEST(AuthModule, OpenIDRedirectCanHandleUpstreamError)
{
    auto auth = std::make_unique<AuthMock>();
    SessionService sessions(*auth);
    AuthModule module(*auth, sessions);
    {
        mw::HTTPServer::Request req;
        req.params.emplace("error", "aaa");
        mw::HTTPServer::Response res;
        module.handleOpenIDRedirect(req, res);
        EXPECT_EQ(res.status, 500);
    }
    {
        mw::HTTPServer::Request req;
        mw::HTTPServer::Response res;
        module.handleOpenIDRedirect(req, res);
        EXPECT_EQ(res.status, 500);
    }
}

TEST(AuthModule, CallbackPropagatesAuthenticationAndIdentityErrors)
{
    AuthMock auth;
    SessionService sessions(auth);
    AuthModule module(auth, sessions);
    EXPECT_CALL(auth, authenticate("expired")).WillOnce(Return(
        std::unexpected(httpError(401, "Code expired"))));
    mw::HTTPServer::Request expired;
    expired.params.emplace("code", "expired");
    mw::HTTPServer::Response rejected;
    module.handleOpenIDRedirect(expired, rejected);
    EXPECT_EQ(rejected.status, 401);
    EXPECT_EQ(rejected.body, "Code expired");
    EXPECT_FALSE(rejected.has_header("Set-Cookie"));

    Tokens tokens;
    tokens.access_token = "access";
    EXPECT_CALL(auth, authenticate("good")).WillOnce(Return(tokens));
    EXPECT_CALL(auth, getUser(tokens)).WillOnce(Return(
        std::unexpected(runtimeError("Identity unavailable"))));
    mw::HTTPServer::Request good;
    good.params.emplace("code", "good");
    mw::HTTPServer::Response failed;
    module.handleOpenIDRedirect(good, failed);
    EXPECT_EQ(failed.status, 500);
    EXPECT_EQ(failed.body, "Identity unavailable");
    EXPECT_FALSE(failed.has_header("Set-Cookie"));
}
