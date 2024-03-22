#include <memory>
#include <string>
#include <regex>
#include <variant>
#include <filesystem>
#include <vector>

#include <inja.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "error.hpp"
#include "http_client.hpp"
#include "url.hpp"
#include "utils.hpp"
#include "weekly.hpp"

#define _ASSIGN_OR_RESPOND_ERROR(tmp, var, val, res)                    \
    auto tmp = val;                                                     \
    if(!tmp.has_value())                                                \
    {                                                                   \
        if(std::holds_alternative<HTTPError>(tmp.error()))              \
        {                                                               \
            const HTTPError& e = std::get<HTTPError>(tmp.error());      \
            res.status = e.code;                                        \
            res.set_content(e.msg, "text/plain");                       \
            return;                                                     \
        }                                                               \
        else                                                            \
        {                                                               \
            res.status = 500;                                           \
            res.set_content(std::visit([](const auto& e) { return e.msg; }, \
                                       tmp.error()),                    \
                            "text/plain");                              \
            return;                                                     \
        }                                                               \
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

void setTokenCookies(const Tokens& tokens, httplib::Response& res)
{
    int64_t expire_sec = 3600*24*30; // One month
    if(tokens.expiration.has_value())
    {
        auto expire = std::chrono::duration_cast<std::chrono::seconds>(
            *tokens.expiration - Clock::now());
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

void copyToHttplibReq(const HTTPRequest& src, httplib::Request& dest)
{
    std::string type = "text/plain";
    if(auto it = src.header.find("Content-Type");
       it != std::end(src.header))
    {
        type = src.header.at("Content-Type");
    }
    dest.set_header("Content-Type", type);
    dest.body = src.request_data;
    for(const auto& [key, value]: src.header)
    {
        if(key != "Content-Type")
        {
            dest.set_header(key, value);
        }
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

nlohmann::json weeklyToJSON(const WeeklyPost& p)
{
    std::string content = *p.render().or_else([](const auto& e) -> E<std::string>
    {
        return errorMsg(e);
    });

    std::chrono::year_month_day date(std::chrono::floor<std::chrono::days>(p.week_begin));
    int week = daysSinceNewYear(p.week_begin);
    std::string week_str = std::format("{} week {}", date.year(), week);
    return {{ "week_str", week_str},
            { "content", std::move(content) },
    };
}

App::App(const Configuration& conf, std::unique_ptr<AuthInterface> openid_auth,
         std::unique_ptr<DataSourceInterface> data_source)
        : config(conf),
          templates((std::filesystem::path(config.data_dir) / "templates" / "")
                    .string()),
          auth(std::move(openid_auth)), data(std::move(data_source))
{
    templates.add_callback("url_for", 2, [&](const inja::Arguments& args)
    {
        return urlFor(args.at(0)->get_ref<const std::string&>(),
                      args.at(1)->get_ref<const std::string&>());
    });

}

std::string App::urlFor(const std::string& name, const std::string& arg) const
{
    if(name == "index")
    {
        return "/";
    }
    if(name == "openid-redirect")
    {
        return "/openid-redirect";
    }
    if(name == "weekly")
    {
        return "/weekly/" + arg;
    }
    if(name == "statics")
    {
        return "/statics/" + arg;
    }
    return "";
}

void App::handleIndexWithInvalidSession(httplib::Response& res) const
{
    switch(config.guest_index)
    {
    case GuestIndex::USER_WEEKLY:
        res.set_redirect(urlFor("weekly", config.guest_index_user), 301);
        return;
    }
    res.status = 500;
    res.set_content("Someone forgot to add a switch case 🤣", "text/plain");
}

void App::handleIndex(const httplib::Request& req, httplib::Response& res) const
{
    E<SessionValidation> session = validateSession(req);
    if(!session.has_value())
    {
        handleIndexWithInvalidSession(res);
        return;
    }

    switch(session->status)
    {
    case SessionValidation::INVALID:
        handleIndexWithInvalidSession(res);
        return;
    case SessionValidation::VALID:
        res.set_redirect(urlFor("weekly", session->user.name), 302);
        return;
    case SessionValidation::REFRESHED:
        setTokenCookies(session->new_tokens, res);
        res.set_redirect(urlFor("weekly", session->user.name), 302);
        return;
    }
}

void App::handleLogin(httplib::Response& res) const
{
    res.set_redirect(auth->initialURL(), 301);
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
    res.set_redirect(urlFor("index", ""), 301);
}

void App::handleUserWeekly(const httplib::Request& req, httplib::Response& res,
                           const std::string& username)
{
    E<SessionValidation> session = validateSession(req);
    std::string session_user;
    if(session.has_value() && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
    }

    ASSIGN_OR_RESPOND_ERROR(std::vector<WeeklyPost> weeklies,
                            data->getWeekliesOneYear(username), res);
    nlohmann::json weeklies_json(nlohmann::json::value_t::array);
    for(const WeeklyPost& p: weeklies)
    {
        weeklies_json.push_back(weeklyToJSON(p));
    }
    nlohmann::json data{{ "weeklies", std::move(weeklies_json) },
                        { "username", username },
                        { "this_url", req.target },
    };
    std::string result = templates.render_file(
        "weeklies.html", std::move(data));
    res.set_content(result, "text/html");
}

void App::start()
{
    httplib::Server server;
    std::string statics_dir = (std::filesystem::path(config.data_dir) /
                               "statics").string();
    spdlog::info("Mounting static dir at {}...", statics_dir);
    auto ret = server.set_mount_point("/statics", statics_dir);
    if (!ret)
    {
        spdlog::error("Failed to mount statics");
    }

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
