#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <format>

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <inja.hpp>

#include "auth.hpp"
#include "config.hpp"
#include "http_client.hpp"

void copyToHttplibReq(const HTTPRequest& src, httplib::Request& dest);

class App
{
public:
    App() = delete;
    explicit App(const Configuration& conf,
                 std::unique_ptr<AuthInterface> openid_auth);

    std::string urlFor(const std::string& name, const std::string& arg) const;

    void handleIndex(const httplib::Request& req, httplib::Response& res) const;
    void handleLogin(httplib::Response& res) const;
    void handleOpenIDRedirect(const httplib::Request& req,
                              httplib::Response& res) const;
    void handleUserWeekly(const httplib::Request& req, httplib::Response& res,
                          const std::string& username) const;
    void start();

private:
    struct SessionValidation
    {
        enum { VALID, REFRESHED, INVALID } status;
        UserInfo user;
        Tokens new_tokens;

        static SessionValidation valid(UserInfo&& user_info)
        {
            return {VALID, user_info, {}};
        }

        static SessionValidation refreshed(UserInfo&& user_info, Tokens&& tokens)
        {
            return {REFRESHED, user_info, tokens};
        }

        static SessionValidation invalid()
        {
            return {INVALID, {}, {}};
        }
    };
    E<SessionValidation> validateSession(const httplib::Request& req) const;

    const Configuration config;
    inja::Environment templates;
    std::unique_ptr<AuthInterface> auth;
};
