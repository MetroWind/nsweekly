#include <memory>
#include <string>
#include <regex>

#include <inja.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "http_client.hpp"

#define _ASSIGN_OR_RESPOND_ERROR(tmp, var, val, res)                    \
    auto tmp = val;                                                     \
    if(!tmp.has_value()) {                                              \
    if(std::holds_alternative<HTTPError>(tmp.error()))                  \
    {                                                                   \
        const HTTPError& e = std::get<HTTPError>(tmp.error());          \
        res.status = e.status;                                          \
        res.set_content(e.msg, "text/plain");                           \
        return;                                                         \
    }                                                                   \
    else                                                                \
    {                                                                   \
        res.status = 500;                                               \
        res.set_content(std::visit([](e) { return e.msg; }, tmp.error()), \
                        "text/plain");                                  \
        return;                                                         \
    }                                                                   \
    }                                                                   \
    var = std::move(tmp).value()

// Val should be a rvalue. Var should be already declared.
#define ASSIGN_OR_RESPOND_ERROR(var, val, res)                          \
    _ASSIGN_OR_RETURN_INNER(_CONCAT_NAMES(assign_or_return_tmp, __COUNTER__), var, val, res)

App::App(const Configuration& conf,
         std::unique_ptr<AuthOpenIDConnect> openid_auth)
        : config(conf), templates(conf.template_dir),
          auth(std::move(openid_auth))
{
}

void App::handleIndex([[maybe_unused]] httplib::Response& res) const
{
    // res.set_redirect(urlForAlbum("", config));
}

void App::handleLogin(httplib::Response& res) const
{
    if(auth == nullptr)
    {
        res.status = 500;
        res.set_content("Null auth", "text/plain");
        return;
    }

    res.set_redirect(auth.initialURL());
}

void App::handleOpenIDRedirect(const httplib::Request& req,
                               httplib::Response& res) const
{
    if(req.has_param("error"))
    {
        res.status = 500;
        if(req.has_param("error_description"))
        {
            res.set_content(
                std::format("{}: {}.", req.get_param_value("error"),
                            req.get_param_value("error_description")),
                "text/plain");
        }
        return;
    }
    else if(!req.has_param("code"))
    {
        res.status = 500;
        res.set_content("No error or code in auth response", "text/plain");
        return;
    }

    std::string code = req.get_param_value("code");

}

void App::start()
{
    httplib::Server server;
    // spdlog::info("Mounting static dir at {}...", config.static_dir);
    // auto ret = server.set_mount_point("/static", config.static_dir);
    // if (!ret)
    // {
    //     spdlog::error("Failed to mount static");
    // }

    server.Get("/", [&]([[maybe_unused]] const httplib::Request& req,
                        httplib::Response& res)
    {
        handleIndex(res);
    });

    server.Get("/login", [&]([[maybe_unused]] const httplib::Request& req,
                        httplib::Response& res)
    {
        handleLogin(res);
    });

    spdlog::info("Listening at http://{}:{}/...", config.listen_address,
                 config.listen_port);
    server.listen(config.listen_address, config.listen_port);
}
