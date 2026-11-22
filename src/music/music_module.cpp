#include <filesystem>
#include <spdlog/spdlog.h>

#include "music_module.hpp"
#include "music_http.hpp"
#include "route_urls.hpp"
#include "text.hpp"
#include "url.hpp"

namespace
{
using Json = nlohmann::json;
using Request = httplib::Request;
using Response = httplib::Response;
using namespace std::chrono;

constexpr int PAGE_SIZE = 50;
constexpr size_t MAX_NONCES = 4096;

void browserError(Response& res, int status, std::string_view message)
{
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(std::string(message), "text/plain; charset=utf-8");
}

void browserError(Response& res, const Error& error)
{
    if(const auto* http = error.as<HTTPError>())
    {
        browserError(res, http->code, http->msg);
    }
    else if(error.is<SQLiteContention>())
    {
        res.set_header("Retry-After", "1");
        browserError(res, 503, "Music storage is temporarily busy");
    }
    else
    {
        spdlog::error("Music browser operation failed");
        browserError(res, 500, "Music operation failed");
    }
}

Json baseContext(const std::string& owner)
{
    return {{"session_user", owner},
            {"attachments_url", urlFor("attachments", owner)},
            {"music_page", true}};
}

struct HistoryFilter
{
    std::string from, to;
};

E<MusicQuery> historyQuery(const Request& req, const time_zone& zone,
                           const HistoryFilter& filter)
{
    if(req.get_param_value_count("from") > 1 ||
       req.get_param_value_count("to") > 1 ||
       req.get_param_value_count("older") > 1)
    {
        return std::unexpected(httpError(400, "Repeated history filter"));
    }
    const auto first = parseCalendarDate(filter.from);
    const auto last = parseCalendarDate(filter.to);
    if((!filter.from.empty() && !first) || (!filter.to.empty() && !last) ||
       (first && last && *first > *last))
    {
        return std::unexpected(
            httpError(400, "Enter a valid date range in YYYY-MM-DD format."));
    }
    MusicQuery query;
    query.limit = PAGE_SIZE + 1;
    // Choose the first occurrence of an ambiguous midnight and the
    // transition instant if midnight was skipped.
    if(first)
    {
        query.range.from =
            zone.to_sys(local_seconds(local_days(*first)), choose::earliest)
                .time_since_epoch().count();
    }
    if(last)
    {
        query.range.to =
            zone.to_sys(local_seconds(local_days(*last) + days(1)),
                        choose::earliest).time_since_epoch().count();
    }
    if(req.has_param("older"))
    {
        const auto cursor = req.get_param_value("older");
        auto separator = cursor.find('.');
        if(separator != std::string::npos)
        {
            auto timestamp = musicInteger(std::string_view(cursor)
                                              .substr(0, separator));
            auto id = musicInteger(std::string_view(cursor)
                                       .substr(separator + 1));
            if(timestamp && *timestamp >= 0 && id && *id > 0)
            {
                query.older = {*timestamp, *id};
            }
        }
        if(!query.older)
        {
            return std::unexpected(httpError(400, "Invalid history cursor."));
        }
    }
    return query;
}

HistoryFilter historyFilter(const Request& req, const time_zone& zone)
{
    HistoryFilter filter{req.get_param_value("from"), req.get_param_value("to")};
    if(req.get_param_value("all") != "1" &&
       !req.has_param("from") && !req.has_param("to"))
    {
        auto today = floor<days>(zone.to_local(
            sys_seconds{seconds(musicEpochSeconds())}));
        filter.from = std::format("{:%F}", year_month_day(today - days(29)));
        filter.to = std::format("{:%F}", year_month_day(today));
    }
    return filter;
}

std::string olderLink(const std::string& url, const HistoryFilter& filter,
                      const MusicListen& last)
{
    return std::format("{}?from={}&to={}&older={}.{}",
                       url, filter.from, filter.to, last.listened_at, last.id);
}

Json displayTrack(const MusicTrack& track)
{
    return {{"artist", track.artist}, {"title", track.title},
            {"release", track.release.value_or("")}};
}

Json displayHistory(const std::vector<MusicListen>& listens,
                    const time_zone& zone)
{
    auto groups = Json::array();
    std::string previous;
    for(const auto& listen : listens)
    {
        auto utc = sys_seconds{seconds(listen.listened_at)};
        auto local = zone.to_local(utc);
        auto date = std::format("{:%F}", floor<days>(local));
        if(date != previous)
        {
            groups.push_back({{"date", date}, {"listens", Json::array()}});
            previous = std::move(date);
        }
        auto entry = displayTrack(listen.track);
        entry["time"] = std::format("{:%R}", local);
        entry["datetime"] = std::format("{:%FT%T}Z", utc);
        groups.back()["listens"].push_back(std::move(entry));
    }
    return groups;
}

Json displayRankings(const std::vector<MusicRanking>& rankings)
{
    auto result = Json::array();
    for(const auto& entry : rankings)
    {
        result.push_back({{"artist", entry.artist}, {"title", entry.title},
                          {"count", entry.count}});
    }
    return result;
}

std::string displayTime(std::optional<int64_t> timestamp,
                        const time_zone& zone)
{
    if(!timestamp) { return {}; }
    return std::format("{:%F %R}", zone.to_local(
        sys_seconds{seconds(*timestamp)}));
}
}

MusicModule::MusicModule(const Configuration& config, MusicDataInterface& data,
                         UserDataInterface& user_data, SessionService& service,
                         const std::chrono::time_zone& time_zone)
    : music(data), users(user_data), sessions(service), zone(time_zone),
      guest_user(config.guest_index_user),
      templates(
          (std::filesystem::path(config.data_dir) / "templates" / "").string())
{
    auto url = URL::fromStr(config.url_prefix);
    if(url)
    {
        const auto api_path = std::string_view(LISTENBRAINZ_API_PATH);
        auto path = std::string(api_path.substr(0, api_path.rfind('/'))) + "/";
        pano_url = url->query(nullptr).fragment(nullptr)
                      .user(nullptr).password(nullptr).appendPath(path).str();
    }
    templates.set_html_autoescape(true);
    templates.add_callback("url_for", 2, [](const inja::Arguments& args)
    {
        return urlFor(args[0]->get<std::string>(), args[1]->get<std::string>());
    });
}

E<std::string> MusicModule::issueNonce(const std::string& owner)
{
    ASSIGN_OR_RETURN(auto nonce, musicNonce());
    std::scoped_lock lock(nonce_mutex);
    const auto now = MusicClock::now();
    std::erase_if(nonces, [now](const auto& entry)
    {
        return entry.second.expires <= now;
    });
    if(nonces.size() >= MAX_NONCES)
    {
        auto oldest = std::ranges::min_element(
            nonces, {}, [](const auto& entry) { return entry.second.expires; });
        nonces.erase(oldest);
    }
    if(!nonces.emplace(nonce, FormNonce{owner, now + minutes(30)}).second)
    {
        return std::unexpected(runtimeError("Could not reserve Music nonce"));
    }
    return nonce;
}

bool MusicModule::checkNonce(const std::string& owner, const std::string& nonce,
                             bool consume)
{
    std::scoped_lock lock(nonce_mutex);
    auto entry = nonces.find(nonce);
    if(entry == nonces.end() || entry->second.owner != owner ||
       entry->second.expires <= MusicClock::now())
    {
        return false;
    }
    if(consume) { nonces.erase(entry); }
    return true;
}

std::optional<std::string> MusicModule::account(const Request& req,
                                                Response& res)
{
    auto session = sessions.validateSession(req);
    if(!session || session->status == SessionValidation::INVALID)
    {
        return std::nullopt;
    }
    if(session->status == SessionValidation::REFRESHED)
    {
        sessions.setTokenCookies(session->new_tokens, res);
    }
    return std::move(session->user.name);
}

std::optional<std::string> MusicModule::requireAccount(const Request& req,
                                                       Response& res)
{
    auto user = account(req, res);
    if(!user)
    {
        res.set_redirect("/login", 303);
    }
    return user;
}

void MusicModule::render(Response& res, const char* path, const Json& context)
{
    try
    {
        std::scoped_lock lock(render_mutex);
        res.set_content(templates.render_file(path, context),
                        "text/html; charset=utf-8");
    }
    catch(const std::exception&)
    {
        browserError(res, runtimeError("Music template rendering failed"));
    }
}

void MusicModule::history(const Request& req, Response& res,
                          const std::string& requested_user)
{
    const auto viewer = account(req, res);
    const auto user = requested_user.empty()
        ? viewer.value_or(guest_user) : requested_user;
    const bool is_owner = viewer && *viewer == user;
    auto owner = users.getUserID(user);
    if(!owner) { browserError(res, owner.error()); return; }
    if(!*owner && !is_owner)
    {
        browserError(res, 404, "User not found.");
        return;
    }

    res.set_header("Cache-Control", "private, no-cache");
    auto filter = historyFilter(req, zone);
    auto query = historyQuery(req, zone, filter);
    auto context = baseContext(viewer.value_or(""));
    const auto history_url = urlFor("music", user);
    context.update(Json{{"username", user}, {"history_url", history_url},
                        {"is_owner", is_owner}, {"zone", zone.name()},
                        {"from", filter.from}, {"to", filter.to}, {"error", ""},
                        {"new_account", false}, {"groups", Json::array()},
                        {"now", nullptr}, {"count", 0},
                        {"artists", Json::array()}, {"tracks", Json::array()},
                        {"older", ""}});
    if(!query)
    {
        res.status = 400;
        context["error"] = errorMsg(query.error());
        render(res, "music/history.html", context);
        return;
    }
    if(!*owner)
    {
        context["new_account"] = true;
        render(res, "music/history.html", context);
        return;
    }
    auto listens = music.history(**owner, *query);
    if(!listens) { browserError(res, listens.error()); return; }
    auto summary = music.summary(**owner, query->range);
    if(!summary) { browserError(res, summary.error()); return; }
    auto playing = music.nowPlaying(**owner);
    if(!playing) { browserError(res, playing.error()); return; }
    if(*playing) { context["now"] = displayTrack(**playing); }
    if(is_owner && summary->count == 0)
    {
        auto devices = music.devices(**owner);
        if(!devices) { browserError(res, devices.error()); return; }
        context["new_account"] = devices->empty();
    }
    if(listens->size() > PAGE_SIZE)
    {
        listens->resize(PAGE_SIZE);
        context["older"] = olderLink(history_url, filter, listens->back());
    }
    context["groups"] = displayHistory(*listens, zone);
    context["count"] = summary->count;
    context["artists"] = displayRankings(summary->artists);
    context["tracks"] = displayRankings(summary->tracks);
    render(res, "music/history.html", context);
}

void MusicModule::settings(const Request& req, Response& res)
{
    res.set_header("Cache-Control", "no-store");
    auto user = requireAccount(req, res);
    if(!user) { return; }
    auto owner = users.getUserID(*user);
    if(!owner) { browserError(res, owner.error()); return; }
    auto items = Json::array();
    if(*owner)
    {
        auto devices = music.devices(**owner);
        if(!devices) { browserError(res, devices.error()); return; }
        for(const auto& item : *devices)
        {
            items.push_back({{"id", item.id}, {"label", item.label},
                             {"created", displayTime(item.created, zone)},
                             {"revoked", displayTime(item.revoked, zone)},
                             {"last", displayTime(item.last_submission, zone)}});
        }
    }
    auto nonce = issueNonce(*user);
    if(!nonce) { browserError(res, nonce.error()); return; }
    auto context = baseContext(*user);
    context.update(Json{{"pano_url", pano_url}, {"nonce", *nonce},
                        {"devices", std::move(items)}, {"zone", zone.name()}});
    render(res, "music/settings.html", context);
}

void MusicModule::device(const Request& req, Response& res,
                         const httplib::ContentReader& reader, bool revoke)
{
    res.set_header("Cache-Control", "no-store");
    auto user = requireAccount(req, res);
    if(!user) { return; }
    if(!musicMediaType(req, "application/x-www-form-urlencoded"))
    {
        browserError(res, 415, "Use the Music settings form.");
        return;
    }
    auto body = musicBody(req, res, reader, 4096);
    if(!body) { browserError(res, body.error()); return; }
    httplib::Params fields;
    httplib::detail::parse_query_text(*body, fields);
    auto nonce = fields.find("csrf");
    if(nonce == fields.end() || fields.count("csrf") != 1 ||
       !checkNonce(*user, nonce->second, !revoke))
    {
        browserError(res, 403, "Form expired. Reload Music settings and retry.");
        return;
    }

    if(revoke)
    {
        auto id = musicInteger(req.matches[1].str());
        auto owner = users.getUserID(*user);
        if(!owner) { browserError(res, owner.error()); return; }
        if(!id || !*owner)
        {
            browserError(res, 404, "Device not found.");
            return;
        }
        auto revoked = music.revokeDevice(**owner, *id, musicEpochSeconds());
        if(!revoked) { browserError(res, revoked.error()); return; }
        if(!*revoked)
        {
            browserError(res, 404, "Device not found.");
            return;
        }
        res.set_redirect("/music/settings", 303);
        return;
    }

    auto field = fields.find("label");
    auto label = field == fields.end() ? "" : musicTrim(field->second);
    if(fields.count("label") != 1 || label.empty() || label.size() > 120 ||
       !validUtf8Text(label))
    {
        browserError(res, 400, "Enter a device name of 1 to 120 UTF-8 bytes.");
        return;
    }
    auto ensured = users.ensureUser(*user);
    if(!ensured) { browserError(res, ensured.error()); return; }
    auto owner = users.getUserID(*user);
    if(!owner) { browserError(res, owner.error()); return; }
    if(!*owner)
    {
        browserError(res, 500, "Music account could not be created.");
        return;
    }
    auto token = musicDeviceToken();
    if(!token) { browserError(res, token.error()); return; }
    auto digest = musicDigest(*token);
    if(!digest) { browserError(res, digest.error()); return; }
    auto saved = music.createDevice(**owner, label, *digest, musicEpochSeconds());
    if(!saved) { browserError(res, saved.error()); return; }
    auto context = baseContext(*user);
    context.update(Json{{"label", label}, {"token", *token},
                        {"pano_url", pano_url}});
    render(res, "music/token.html", context);
}

void MusicModule::registerRoutes(httplib::Server& server)
{
    server.Get("/music", [this](const auto& req, auto& res)
    {
        history(req, res, {});
    });
    server.Get(R"(/music/user/([^/]+))", [this](const auto& req, auto& res)
    {
        history(req, res, req.matches[1].str());
    });
    server.Get("/music/settings", [this](const auto& req, auto& res)
    {
        settings(req, res);
    });
    server.Post("/music/devices",
                [this](const auto& req, auto& res, const auto& reader)
    {
        device(req, res, reader, false);
    });
    server.Post(R"(/music/devices/([1-9][0-9]*)/revoke)",
                [this](const auto& req, auto& res, const auto& reader)
    {
        device(req, res, reader, true);
    });
}
