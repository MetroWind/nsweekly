#include <string>
#include <regex>

#include <inja.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "config.hpp"

App::App(const Configuration& conf)
        : config(conf), templates(conf.template_dir)
{
}

void App::handleIndex([[maybe_unused]] httplib::Response& res) const
{
    // res.set_redirect(urlForAlbum("", config));
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

    spdlog::info("Listening at http://{}:{}/...", config.listen_address,
                 config.listen_port);
    server.listen(config.listen_address, config.listen_port);
}
