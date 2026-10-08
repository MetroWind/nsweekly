#include <algorithm>
#include <charconv>
#include <filesystem>
#include <set>
#include <spdlog/spdlog.h>
#include "games_module.hpp"
#include "game_markdown.hpp"
#include "route_urls.hpp"
#include "url.hpp"

namespace
{
using Request = mw::HTTPServer::Request;
using Response = mw::HTTPServer::Response;
constexpr size_t FORM_BODY_LIMIT = 1024 * 1024;

void htmlError(Response &res, int status)
{
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(
        std::format("<!doctype html><html lang=\"en\"><title>Error</title>"
                    "<h1>Request failed ({})</h1><p>Please check the request "
                    "or try again."
                    "</p></html>",
                    status),
        "text/html; charset=utf-8");
}

void storageError(Response &res, const Error &error)
{
    if(const auto *http = error.as<HTTPError>())
    {
        htmlError(res, http->code);
    }
    else
    {
        spdlog::error("Games storage operation failed");
        htmlError(res, 500);
    }
}

std::string origin(const std::string &text)
{
    auto url = URL::fromStr(text);
    if(!url || (url->scheme() != "http" && url->scheme() != "https") ||
       !url->user().empty() || !url->password().empty())
    {
        return "";
    }
    auto port = url->port();
    if((url->scheme() == "http" && port == "80") ||
       (url->scheme() == "https" && port == "443"))
    {
        port.clear();
    }
    return url->scheme() + "://" + url->host() +
           (port.empty() ? "" : ":" + port);
}

E<GameFields> parseFields(const Request &req, bool deletion)
{
    if(req.target.find('?') != std::string::npos)
    {
        return std::unexpected(httpError(400, "Query action data forbidden"));
    }
    const std::set<std::string> allowed{"name",       "platforms", "status",
                                        "completion", "hours",     "start_date",
                                        "end_date",   "notes"};
    GameFields fields;
    // Parse each pair with httplib separately: its whole-body parser
    // deduplicates identical pairs, which would hide repeated scalars.
    size_t offset = 0;
    while(offset < req.body.size())
    {
        auto end = req.body.find('&', offset);
        if(end == std::string::npos)
        {
            end = req.body.size();
        }
        std::string pair = req.body.substr(offset, end - offset);
        for(size_t i = 0; i < pair.size(); ++i)
        {
            if(pair[i] != '%')
            {
                continue;
            }
            int value = 0;
            if(i + 2 >= pair.size() ||
               !httplib::detail::from_hex_to_i(pair, i + 1, 2, value))
            {
                return std::unexpected(httpError(400, "Malformed encoding"));
            }
            i += 2;
        }
        httplib::Params params;
        httplib::detail::parse_query_text(pair, params);
        if(params.size() != 1)
        {
            return std::unexpected(httpError(400, "Malformed form"));
        }
        const auto &[key, value] = *params.begin();
        if(deletion || !allowed.contains(key))
        {
            return std::unexpected(httpError(400, "Unexpected field"));
        }
        if(key == "platforms")
        {
            fields.platforms.push_back(value);
        }
        else if(!fields.scalars.emplace(key, value).second)
        {
            return std::unexpected(httpError(400, "Repeated scalar"));
        }
        offset = end + 1;
    }
    return fields;
}

std::string_view platformGlyph(GamePlatform platform)
{
    switch(platform)
    {
    case GamePlatform::PC:
        return "\U000f0aab";
    case GamePlatform::SWITCH:
    case GamePlatform::SWITCH_2:
        return "\U000f07e1";
    case GamePlatform::PS_5:
        return "\U000f0414";
    case GamePlatform::EMULATOR:
        return "\U000f0297";
    }
    return {};
}

template <typename T, size_t N>
nlohmann::json choices(const std::array<GameChoice<T>, N> &list,
                       const std::vector<std::string> &selected)
{
    auto result = nlohmann::json::array();
    for(const auto &choice : list)
    {
        result.push_back(
            {{"code", choice.code},
             {"label", choice.label},
             {"selected",
              std::ranges::find(selected, choice.code) != selected.end()}});
    }
    return result;
}
} // namespace

GamesModule::GamesModule(const Configuration &conf, GameDataInterface &data,
                         UserDataInterface &user_data, SessionService &service)
    : config(conf), games(data), users(user_data), sessions(service),
      templates(
          (std::filesystem::path(conf.data_dir) / "templates" / "").string())
{
    templates.add_callback("url_for", 2,
                           [](const inja::Arguments &args)
                           {
                               return gameEscape(
                                   urlFor(args[0]->get<std::string>(),
                                          args[1]->get<std::string>()));
                           });
}

void GamesModule::handleIndex(const Request &req, Response &res)
{
    res.set_header("Cache-Control", "no-store");
    auto session = sessions.validateSession(req);
    if(!session || session->status == SessionValidation::INVALID)
    {
        res.set_redirect("/login", 302);
        return;
    }
    if(session->status == SessionValidation::REFRESHED)
    {
        sessions.setTokenCookies(session->new_tokens, res);
    }
    res.set_redirect(gamesURL(session->user.name), 302);
}

void GamesModule::handle(const Request &req, Response &res,
                         const std::string &user, const std::string &action,
                         const std::string &id_text)
{
    auto session = sessions.validateSession(req);
    handleSession(req, res, user, action, id_text, session);
}

void GamesModule::handleSession(const Request &req, Response &res,
                                const std::string &user,
                                const std::string &action,
                                const std::string &id_text,
                                const E<SessionValidation> &session)
{
    res.status = 200;
    res.set_header("Cache-Control", "no-store");
    std::string session_user;
    if(session && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
        if(session->status == SessionValidation::REFRESHED)
        {
            sessions.setTokenCookies(session->new_tokens, res);
        }
    }
    bool owner = !session_user.empty() && session_user == user;
    if(!action.empty())
    {
        if(session_user.empty())
        {
            htmlError(res, 401);
            return;
        }
        if(!owner)
        {
            htmlError(res, 403);
            return;
        }
    }
    int64_t id = 0;
    if(action == "edit" || action == "delete")
    {
        auto [end, ec] = std::from_chars(id_text.data(),
                                         id_text.data() + id_text.size(), id);
        if(ec != std::errc{} || end != id_text.data() + id_text.size() ||
           id <= 0)
        {
            htmlError(res, 400);
            return;
        }
    }
    if(req.method == "POST")
    {
        const auto supplied = req.get_header_value("Origin");
        if(req.get_header_value_count("Origin") != 1 || supplied.empty() ||
           supplied != origin(supplied) ||
           origin(supplied) != origin(config.url_prefix))
        {
            htmlError(res, 403);
            return;
        }
        if(httplib::detail::extract_media_type(req.get_header_value(
               "Content-Type")) != "application/x-www-form-urlencoded")
        {
            htmlError(res, 415);
            return;
        }
        if(req.body.size() > FORM_BODY_LIMIT)
        {
            htmlError(res, 413);
            return;
        }
    }
    if(owner)
    {
        auto ensured = users.ensureUser(user);
        if(!ensured)
        {
            storageError(res, ensured.error());
            return;
        }
    }
    auto uid = users.getUserID(user);
    if(!uid)
    {
        storageError(res, uid.error());
        return;
    }
    if(!*uid)
    {
        htmlError(res, 404);
        return;
    }
    GameFields fields = gameFields(GameInput{});
    // New forms intentionally require an explicit status selection.
    fields.scalars["status"] = "";
    if(id)
    {
        auto record = games.getGame(user, id);
        if(!record)
        {
            storageError(res, record.error());
            return;
        }
        if(!*record)
        {
            htmlError(res, 404);
            return;
        }
        fields = gameFields((*record)->input);
    }
    std::map<std::string, std::string> errors;
    if(req.method == "POST")
    {
        auto parsed = parseFields(req, action == "delete");
        if(!parsed)
        {
            storageError(res, parsed.error());
            return;
        }
        if(action == "delete")
        {
            auto removed = games.deleteGame(user, id);
            if(!removed)
            {
                storageError(res, removed.error());
                return;
            }
            if(!*removed)
            {
                htmlError(res, 404);
                return;
            }
            res.set_redirect(gamesURL(user), 303);
            return;
        }
        fields = std::move(*parsed);
        auto input = validateGameInput(fields);
        if(!input)
        {
            if(const auto *validation = input.error().as<GameValidationError>())
            {
                errors = validation->fields;
                res.status = 422;
            }
            else
            {
                storageError(res, input.error());
                return;
            }
        }
        else
        {
            auto saved = id ? games.updateGame(user, id, *input)
                            : games.createGame(user, *input);
            if(!saved)
            {
                storageError(res, saved.error());
                return;
            }
            res.set_redirect(gamesURL(user), 303);
            return;
        }
    }
    render(res, user, session_user, action, id, fields, errors);
}

void GamesModule::render(Response &res, const std::string &user,
                         const std::string &session_user,
                         const std::string &action, int64_t id,
                         const GameFields &fields,
                         const std::map<std::string, std::string> &errors)
{
    auto records = games.listGames(user);
    if(!records)
    {
        storageError(res, records.error());
        return;
    }
    nlohmann::json rows = nlohmann::json::array();
    for(const auto &record : *records)
    {
        const auto &input = record.input;
        auto raw = gameFields(input);
        std::string platforms;
        auto platform_icons = nlohmann::json::array();
        for(auto platform : input.platforms)
        {
            if(!platforms.empty())
            {
                platforms += ", ";
            }
            platforms += PLATFORM_CHOICES[static_cast<size_t>(platform)].label;
            platform_icons.push_back({
                {"label", gameEscape(std::string(
                    PLATFORM_CHOICES[static_cast<size_t>(platform)].label))},
                {"glyph", platformGlyph(platform)},
                {"is_switch_2", platform == GamePlatform::SWITCH_2}});
        }
        auto cells = nlohmann::json::array();
        cells.push_back({{"value", gameEscape(input.name)},
                         {"key", gameEscape(input.name)},
                         {"missing", false}});
        cells.push_back({{"value", ""},
                         {"key", gameEscape(platforms)},
                         {"missing", platforms.empty()}});
        cells.push_back(
            {{"value", STATUS_CHOICES[static_cast<size_t>(input.status)].label},
             {"key", std::to_string(static_cast<int>(input.status))},
             {"missing", false}});
        cells.push_back(
            {{"value",
              input.completion
                  ? std::string(COMPLETION_CHOICES[static_cast<size_t>(
                                                       *input.completion)]
                                    .label)
                  : ""},
             {"key", input.completion
                         ? std::to_string(static_cast<int>(*input.completion))
                         : ""},
             {"missing", !input.completion}});
        for(const auto &key : {"hours", "start_date", "end_date"})
        {
            const auto &value = raw.scalars.at(key);
            cells.push_back({{"value", gameEscape(value)},
                             {"key", gameEscape(value)},
                             {"missing", value.empty()}});
        }
        std::string html;
        if(input.notes)
        {
            auto rendered = renderGameMarkdown(*input.notes);
            if(rendered)
            {
                html = std::move(*rendered);
            }
            else
            {
                html = "<small>Notes rendering failed</small><pre>" +
                       gameEscape(*input.notes) + "</pre>";
            }
        }
        cells.push_back({{"value", html},
                         {"key", gameEscape(input.notes.value_or(""))},
                         {"missing", !input.notes}});
        const auto base = gamesURL(user) + "/" + std::to_string(record.id);
        rows.push_back({{"id", std::to_string(record.id)},
                        {"cells", cells},
                        {"platforms", platform_icons},
                        {"edit_url", gameEscape(base + "/edit")},
                        {"delete_url", gameEscape(base + "/delete")}});
    }
    nlohmann::json form = nlohmann::json::object();
    nlohmann::json field_errors = nlohmann::json::object();
    for(const auto &key : {"name", "status", "completion", "hours",
                           "start_date", "end_date", "notes", "platforms"})
    {
        auto value = fields.scalars.find(key);
        form[key] =
            value == fields.scalars.end() ? "" : gameEscape(value->second);
        auto error = errors.find(key);
        field_errors[key] =
            error == errors.end() ? "" : gameEscape(error->second);
    }
    auto status = fields.scalars.find("status");
    auto completion = fields.scalars.find("completion");
    auto table_url = gamesURL(user);
    auto action_url =
        table_url + (id ? "/" + std::to_string(id) : "") + "/" + action;
    nlohmann::json context{
        {"username", gameEscape(user)},
        {"session_user", gameEscape(session_user)},
        {"is_owner", !session_user.empty() && session_user == user},
        {"table_url", gameEscape(table_url)},
        {"weekly_url", gameEscape(weeklyURL(user))},
        {"new_url", gameEscape(table_url + "/new")},
        {"action_url", gameEscape(action_url)},
        {"action", action},
        {"games", rows},
        {"form", form},
        {"errors", field_errors},
        {"platforms", choices(PLATFORM_CHOICES, fields.platforms)},
        {"statuses",
         choices(STATUS_CHOICES,
                 {status == fields.scalars.end() ? "" : status->second})},
        {"completions",
         choices(
             COMPLETION_CHOICES,
             {completion == fields.scalars.end() ? "" : completion->second})}};
    try
    {
        std::lock_guard lock(template_mutex);
        res.set_content(templates.render_file("games.html", context),
                        "text/html; charset=utf-8");
    }
    catch(const inja::InjaError &)
    {
        spdlog::error("Games template rendering failed");
        htmlError(res, 500);
    }
}

void GamesModule::receive(const Request &req, Response &res,
                          const httplib::ContentReader &reader,
                          const std::string &user, const std::string &action,
                          const std::string &id_text)
{
    auto session = sessions.validateSession(req);
    res.set_header("Cache-Control", "no-store");
    if(session && session->status == SessionValidation::REFRESHED)
    {
        sessions.setTokenCookies(session->new_tokens, res);
    }
    if(!session || session->status == SessionValidation::INVALID)
    {
        htmlError(res, 401);
        return;
    }
    if(session->user.name != user)
    {
        htmlError(res, 403);
        return;
    }
    const auto supplied = req.get_header_value("Origin");
    if(req.get_header_value_count("Origin") != 1 || supplied.empty() ||
       supplied != origin(supplied) ||
       origin(supplied) != origin(config.url_prefix))
    {
        htmlError(res, 403);
        return;
    }
    if(httplib::detail::extract_media_type(req.get_header_value(
           "Content-Type")) != "application/x-www-form-urlencoded")
    {
        htmlError(res, 415);
        return;
    }
    Request bounded = req;
    bounded.body.clear();
    bool oversized = false;
    bool read = reader(
        [&bounded, &oversized](const char *data, size_t size)
        {
            if(size > FORM_BODY_LIMIT - bounded.body.size())
            {
                oversized = true;
                return false;
            }
            bounded.body.append(data, size);
            return true;
        });
    if(oversized)
    {
        htmlError(res, 413);
        return;
    }
    if(!read)
    {
        htmlError(res, 400);
        return;
    }
    // The session was already refreshed before receiving the body.
    // handleSession applies cookies once on its response.
    res.headers.erase("Set-Cookie");
    handleSession(bounded, res, user, action, id_text, session);
}

void GamesModule::dispatch(const Request &req, Response &res,
                           const httplib::ContentReader *reader)
{
    // Split the encoded target before decoding: httplib decodes %2F in
    // Request::path before matching routes, including inside usernames.
    auto path = req.target.substr(7, req.target.find('?') == std::string::npos
                                         ? std::string::npos
                                         : req.target.find('?') - 7);
    auto slash = path.find('/');
    auto user = httplib::decode_path_component(path.substr(0, slash));
    if(user.empty() || !validGameText(user))
    {
        htmlError(res, 400);
        return;
    }
    std::string action;
    std::string id;
    if(slash != std::string::npos)
    {
        auto suffix = path.substr(slash + 1);
        if(suffix == "new")
        {
            action = "new";
        }
        else
        {
            auto separator = suffix.find('/');
            if(separator == std::string::npos)
            {
                htmlError(res, 404);
                return;
            }
            id = suffix.substr(0, separator);
            action = suffix.substr(separator + 1);
            if(action != "edit" && action != "delete")
            {
                htmlError(res, 404);
                return;
            }
        }
    }
    if(reader)
    {
        if(action.empty())
        {
            htmlError(res, 404);
            return;
        }
        receive(req, res, *reader, user, action, id);
    }
    else
    {
        handle(req, res, user, action, id);
    }
}

void GamesModule::registerRoutes(httplib::Server &server)
{
    server.Get("/games",
               [](const Request &, Response &res)
               {
                   res.set_redirect("/games/", 308);
               });
    server.Get("/games/",
               [this](const Request &req, Response &res)
               {
                   handleIndex(req, res);
               });
    for(const auto &path :
        {"/games/:username/new", "/games/:username/:id/edit",
         "/games/:username/:id/delete", "/games/:username", "/games/.*"})
    {
        server.Get(path,
                   [this](const Request &req, Response &res)
                   {
                       dispatch(req, res);
                   });
    }
    for(const auto &path : {"/games/:username/new", "/games/:username/:id/edit",
                            "/games/:username/:id/delete", "/games/.*"})
    {
        server.Post(path,
                    [this](const Request &req, Response &res,
                           const httplib::ContentReader &reader)
                    {
                        dispatch(req, res, &reader);
                    });
    }
}
