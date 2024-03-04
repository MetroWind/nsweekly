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

class App
{
public:
    App() = delete;
    explicit App(const Configuration& conf,
                 std::unique_ptr<AuthOpenIDConnect> openid_auth);

    void handleIndex(httplib::Response& res) const;
    void handleLogin(httplib::Response& res) const;
    void handleOpenIDRedirect(const httplib::Request& req,
                              httplib::Response& res) const;
    void start();

private:
    const Configuration config;
    inja::Environment templates;
    std::unique_ptr<AuthOpenIDConnect> auth;
};
