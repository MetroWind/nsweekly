#include <memory>
#include <string>
#include <regex>
#include <variant>

#include <inja.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "http_client.hpp"
#include "utils.hpp"
#include "url.hpp"

#define _ASSIGN_OR_RESPOND_ERROR(tmp, var, val, res)                    \
    auto tmp = val;                                                     \
    if(!tmp.has_value()) {                                              \
    if(std::holds_alternative<HTTPError>(tmp.error()))                  \
    {                                                                   \
        const HTTPError& e = std::get<HTTPError>(tmp.error());          \
        res.status = e.code;                                          \
        res.set_content(e.msg, "text/plain");                           \
        return;                                                         \
    }                                                                   \
    else                                                                \
    {                                                                   \
        res.status = 500;                                               \
        res.set_content(std::visit([](const auto& e) { return e.msg; }, \
                                   tmp.error()),                    \
                        "text/plain");                                  \
        return;                                                         \
    }                                                                   \
    }                                                                   \
    var = std::move(tmp).value()

// Val should be a rvalue.
#define ASSIGN_OR_RESPOND_ERROR(var, val, res)                          \
    _ASSIGN_OR_RESPOND_ERROR(_CONCAT_NAMES(assign_or_return_tmp, __COUNTER__), \
                            var, val, res)

std::unordered_map<std::string, std::string> parseCookies(std::string_view value)
{
    std::unordered_map<std::string, std::string> cookies;
    size_t begin = 0;
    while(true)
    {
        if(begin >= value.size())
        {
            break;
        }

        size_t semicolon = value.find(';', begin);
        if(semicolon == std::string::npos)
        {
            semicolon = value.size();
        }

        std::string_view section = value.substr(begin, semicolon);

        begin = semicolon + 1;
        // Skip spaces
        while(begin < value.size() && value[begin] == ' ')
        {
            begin++;
        }

        size_t equal = section.find('=');
        if(equal == std::string::npos) continue;
        cookies.emplace(section.substr(0, equal),
                        section.substr(equal+1, semicolon));
        if(semicolon >= value.size())
        {
            continue;
        }
    }
    return cookies;
}

std::string urlFor(const std::string& name,
                   [[maybe_unused]] const std::string& arg,
                   [[maybe_unused]] const Configuration& config)
{
    if(name == "index")
    {
        return "/";
    }
    if(name == "openid-redirect")
    {
        return "/openid-redirect";
    }
    return "";
}

void setTokenCookies(const Tokens& tokens, httplib::Response& res)
{
    int64_t expire_sec = 3600*24*30; // One month
    if(tokens.expiration.has_value())
    {
        auto expire = std::chrono::duration_cast<std::chrono::seconds>(
            *tokens.expiration - std::chrono::steady_clock::now());
        expire_sec = expire.count();
    }
    res.set_header("Set-Cookie", std::format(
                       "access-token={}; Max-Age={}",
                       urlEncode(tokens.access_token), expire_sec));
    // Add refresh token to cookie, with one month expiration.
    if(tokens.refresh_token.has_value())
    {
        res.set_header("Set-Cookie", std::format(
                           "refresh-token={}; Max-Age={}",
                           urlEncode(*tokens.refresh_token), 3600*24*30));
    }
}

E<App::SessionValidation> App::validateSession(const httplib::Request& req) const
{
    if(!req.has_header("Cookie"))
    {
        spdlog::debug("Request has no cookie.");
        return SessionValidation::invalid();
    }

    auto cookies = parseCookies(req.get_header_value("Cookie"));
    if(auto it = cookies.find("access-token");
       it != std::end(cookies))
    {
        spdlog::debug("Cookie has access token.");
        Tokens tokens;
        tokens.access_token = it->second;
        E<UserInfo> user = auth->getUser(tokens);
        if(user.has_value())
        {
            return SessionValidation::valid(*std::move(user));
        }
    }
    // No access token or access token expired
    if(auto it = cookies.find("refresh-token");
       it != std::end(cookies))
    {
        spdlog::debug("Cookie has refresh token.");
        // Try to refresh the tokens.
        ASSIGN_OR_RETURN(Tokens tokens, auth->refreshTokens(it->second));
        ASSIGN_OR_RETURN(UserInfo user, auth->getUser(tokens));
        return SessionValidation::refreshed(std::move(user), std::move(tokens));
    }
    return SessionValidation::invalid();
}

App::App(const Configuration& conf,
         std::unique_ptr<AuthOpenIDConnect> openid_auth)
        : config(conf), templates(conf.template_dir),
          auth(std::move(openid_auth))
{
}

void App::handleIndex(const httplib::Request& req, httplib::Response& res) const
{
    E<SessionValidation> session = validateSession(req);
    if(!session.has_value())
    {
        res.set_content(std::string("Not logged in: ") +
                        errorMsg(session.error()), "text/plain");
        return;
    }

    switch(session->status)
    {
    case SessionValidation::INVALID:
        res.set_content("Not logged in", "text/plain");
        return;
    case SessionValidation::VALID:
        res.set_content(std::format("Loggined as {}", session->user.name),
                        "text/plain");
        return;
    case SessionValidation::REFRESHED:
        setTokenCookies(session->new_tokens, res);
        res.set_content(
            std::format("Refreshed session as {}", session->user.name),
            "text/plain");
    }
}

void App::handleLogin(httplib::Response& res) const
{
    if(auth == nullptr)
    {
        res.status = 500;
        res.set_content("Null auth", "text/plain");
        return;
    }

    res.set_redirect(auth->initialURL());
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
    spdlog::debug("OpenID server visited {} with code {}.", req.path, code);
    ASSIGN_OR_RESPOND_ERROR(Tokens tokens, auth->authenticate(code), res);
    ASSIGN_OR_RESPOND_ERROR(UserInfo user, auth->getUser(tokens), res);

    setTokenCookies(tokens, res);
    res.set_redirect(urlFor("index", "", config));
}

void App::handleUserWeekly(const httplib::Request& req, httplib::Response& res,
                           const std::string& username) const
{
    E<SessionValidation> session = validateSession(req);
    std::string session_user;
    if(session.has_value() && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
    }
    // WIP...
    res.set_content(username, "plain/text");
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

    server.Get("/", [&](const httplib::Request& req,
                        httplib::Response& res)
    {
        handleIndex(req, res);
    });

    server.Get("/login", [&]([[maybe_unused]] const httplib::Request& req,
                             httplib::Response& res)
    {
        handleLogin(res);
    });

    server.Get("/openid-redirect", [&](const httplib::Request& req,
                                       httplib::Response& res)
    {
        handleOpenIDRedirect(req, res);
    });

    server.Get("/weekly/:username", [&](const httplib::Request& req,
                                        httplib::Response& res)
    {
        handleUserWeekly(req, res, req.path_params.at("username"));
    });

    spdlog::info("Listening at http://{}:{}/...", config.listen_address,
                 config.listen_port);
    server.listen(config.listen_address, config.listen_port);
}
