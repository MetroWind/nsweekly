#pragma once

#include <string>
#include <string_view>
#include <format>

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <inja.hpp>

#include "config.hpp"

class App
{
public:
    App() = delete;
    explicit App(const Configuration& conf);

    void handleIndex(httplib::Response& res) const;
    void handleOpenIDRedirect(const httplib::Request& req,
                              httplib::Response& res) const;
    void start();

private:
    const Configuration config;
    inja::Environment templates;
};
