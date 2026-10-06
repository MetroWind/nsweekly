#include <spdlog/spdlog.h>

#include "auth_module.hpp"
#include "route_urls.hpp"
#include "http_response.hpp"

void AuthModule::handleLogin(httplib::Response& res) const
{
    res.set_redirect(auth.initialURL(), 301);
}

void AuthModule::handleOpenIDRedirect(const httplib::Request& req,
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
    spdlog::debug("OpenID server visited {} with code {}.", req.path, code);
    ASSIGN_OR_RESPOND_ERROR(Tokens tokens, auth.authenticate(code), res);
    ASSIGN_OR_RESPOND_ERROR(UserInfo user, auth.getUser(tokens), res);

    sessions.setTokenCookies(tokens, res);
    res.set_redirect(urlFor("index", ""), 301);
}

void AuthModule::registerRoutes(httplib::Server& server)
{
    server.Get("/login", [this]([[maybe_unused]] const httplib::Request& req,
                             httplib::Response& res)
    {
        handleLogin(res);
    });

    server.Get("/openid-redirect", [this](const httplib::Request& req,
                                       httplib::Response& res)
    {
        handleOpenIDRedirect(req, res);
    });

}
