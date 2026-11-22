#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>

#include "listenbrainz_api.hpp"
#include "music_http.hpp"
#include "text.hpp"

namespace
{
using Json = nlohmann::json;
using Request = httplib::Request;
using Response = httplib::Response;

constexpr size_t BODY_LIMIT = 10240000;
constexpr size_t ITEM_LIMIT = 10240;
constexpr size_t BATCH_LIMIT = 1000;
constexpr size_t DEPTH_LIMIT = 32;
constexpr int64_t MIN_LISTENED_AT = 1033430400;

enum class ListenType { SINGLE, IMPORT, PLAYING_NOW };

struct Submission
{
    ListenType type;
    std::vector<MusicListen> listens;
    bool return_msid;
};

void send(Response& res, const Json& body, int status = 200)
{
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(), "application/json");
}

void fail(Response& res, int status, std::string_view message)
{
    send(res, {{"code", status}, {"error", message}}, status);
}

void fail(Response& res, const Error& error)
{
    if(const auto* http = error.as<HTTPError>())
    {
        fail(res, http->code, http->msg);
    }
    else if(error.is<SQLiteContention>())
    {
        res.set_header("Retry-After", "1");
        fail(res, 503, "Music storage is temporarily busy");
    }
    else
    {
        fail(res, 500, "Music operation failed");
    }
}

// Bounds nesting and the original byte spans of object items in the payload.
// The JSON parser still validates the complete document and all value types.
E<void> checkJsonLimits(std::string_view body)
{
    size_t depth = 0, payload_depth = 0, item_start = 0, count = 0;
    bool payload_key = false;
    for(size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        if(payload_depth && depth == payload_depth &&
           std::string_view("{], \t\r\n").find(c) == std::string_view::npos)
        {
            return std::unexpected(
                httpError(400, "Each listen must be an object"));
        }
        if(c == '"')
        {
            const size_t start = i++;
            for(; i < body.size() && body[i] != '"'; ++i)
            {
                if(body[i] == '\\')
                {
                    ++i;
                }
            }
            if(i >= body.size())
            {
                break;
            }
            if(depth == 1 && i - start <= 43)
            {
                auto key = Json::parse(body.substr(start, i - start + 1),
                                       nullptr, false);
                payload_key = key == "payload";
            }
            continue;
        }
        if(c == '{' || c == '[')
        {
            if(depth == 1)
            {
                if(payload_key && c == '[')
                {
                    payload_depth = 2;
                    count = 0;
                }
                payload_key = false;
            }
            if(depth == payload_depth && payload_depth && c == '{')
            {
                item_start = i;
                if(++count > BATCH_LIMIT)
                {
                    return std::unexpected(httpError(400, "Too many listens"));
                }
            }
            if(++depth > DEPTH_LIMIT)
            {
                return std::unexpected(httpError(400, "JSON nesting exceeds 32"));
            }
        }
        else if((c == '}' || c == ']') && depth)
        {
            --depth;
            if(payload_depth && depth == payload_depth && c == '}' &&
               i - item_start + 1 > ITEM_LIMIT)
            {
                return std::unexpected(httpError(400, "Listen is too large"));
            }
            if(depth < payload_depth)
            {
                payload_depth = 0;
            }
        }
        else if(depth == 1 && c == ',')
        {
            payload_key = false;
        }
    }
    return {};
}

E<std::string> requiredText(const Json& metadata, const char* field)
{
    const auto value = metadata.find(field);
    if(value != metadata.end() && value->is_string())
    {
        auto trimmed = musicTrim(value->get_ref<const std::string&>());
        if(!trimmed.empty() && validUtf8Text(trimmed))
        {
            return trimmed;
        }
    }
    return std::unexpected(httpError(400, std::string("Invalid ") + field));
}

E<MusicListen> parseListen(const Json& item, ListenType type, int64_t now)
{
    if(!item.is_object())
    {
        return std::unexpected(httpError(400, "Each listen must be an object"));
    }
    const auto metadata = item.find("track_metadata");
    if(metadata == item.end() || !metadata->is_object())
    {
        return std::unexpected(httpError(400, "Invalid track_metadata"));
    }
    ASSIGN_OR_RETURN(auto artist, requiredText(*metadata, "artist_name"));
    ASSIGN_OR_RETURN(auto title, requiredText(*metadata, "track_name"));
    MusicTrack track{std::move(artist), std::move(title), {}, {}};
    if(const auto release = metadata->find("release_name");
       release != metadata->end())
    {
        if(!release->is_string() ||
           !validUtf8Text(release->get_ref<const std::string&>()))
        {
            return std::unexpected(httpError(400, "Invalid release_name"));
        }
        track.release = release->get<std::string>();
    }
    if(const auto extra = metadata->find("additional_info");
       extra != metadata->end())
    {
        if(!extra->is_object())
        {
            return std::unexpected(httpError(400, "Invalid additional_info"));
        }
        track.additional_info = extra->dump();
        if(track.additional_info->size() > ITEM_LIMIT)
        {
            return std::unexpected(httpError(400, "Listen is too large"));
        }
    }
    const auto timestamp = item.find("listened_at");
    if(type == ListenType::PLAYING_NOW)
    {
        if(timestamp != item.end())
        {
            return std::unexpected(
                httpError(400, "Playing-now listens omit listened_at"));
        }
        return MusicListen{0, std::move(track)};
    }
    if(timestamp == item.end() || !timestamp->is_number_integer() ||
       (timestamp->is_number_unsigned() &&
        timestamp->get<uint64_t>() > static_cast<uint64_t>(now + 300)))
    {
        return std::unexpected(httpError(400, "Invalid listened_at"));
    }
    const auto seconds = timestamp->get<int64_t>();
    if(seconds < MIN_LISTENED_AT || seconds > now + 300)
    {
        return std::unexpected(httpError(400, "Invalid listened_at"));
    }
    return MusicListen{seconds, std::move(track)};
}

E<Submission> parseSubmission(const Request& req, std::string_view body,
                              int64_t now)
{
    DO_OR_RETURN(checkJsonLimits(body));
    const auto document = Json::parse(body,
        [](int depth, Json::parse_event_t event, Json& value)
        {
            return depth != 1 || event != Json::parse_event_t::key ||
                   value == "listen_type" || value == "payload";
        }, false);
    if(!document.is_object())
    {
        return std::unexpected(httpError(400, "Expected a JSON object"));
    }
    const auto kind = document.find("listen_type");
    ListenType type;
    if(kind == document.end())
    {
        return std::unexpected(httpError(400, "Invalid listen_type"));
    }
    if(*kind == "single") { type = ListenType::SINGLE; }
    else if(*kind == "import") { type = ListenType::IMPORT; }
    else if(*kind == "playing_now") { type = ListenType::PLAYING_NOW; }
    else { return std::unexpected(httpError(400, "Invalid listen_type")); }

    bool return_msid = false;
    if(const auto options = req.get_param_value_count("return_msid"); options)
    {
        const auto value = req.get_param_value("return_msid");
        if(options != 1 ||
           (value != "true" && value != "1" && value != "false" && value != "0"))
        {
            return std::unexpected(httpError(400, "Invalid return_msid"));
        }
        return_msid =
            type == ListenType::PLAYING_NOW && (value == "true" || value == "1");
    }

    const auto payload = document.find("payload");
    if(payload == document.end() || !payload->is_array() || payload->empty() ||
       payload->size() > BATCH_LIMIT ||
       (type != ListenType::IMPORT && payload->size() != 1))
    {
        return std::unexpected(httpError(400, "Invalid payload size or type"));
    }
    Submission result{type, {}, return_msid};
    result.listens.reserve(payload->size());
    for(const auto& item : *payload)
    {
        ASSIGN_OR_RETURN(auto listen, parseListen(item, type, now));
        result.listens.push_back(std::move(listen));
    }
    return result;
}

E<MusicQuery> readQuery(const Request& req)
{
    MusicQuery query;
    if(req.has_param("min_ts") && req.has_param("max_ts"))
    {
        return std::unexpected(httpError(400, "Use one timestamp bound"));
    }
    for(const auto key : {"count", "min_ts", "max_ts"})
    {
        auto total = req.get_param_value_count(key);
        if(!total) { continue; }
        const auto number = musicInteger(req.get_param_value(key));
        if(total != 1 || !number ||
           (std::string_view(key) == "count" && *number < 0))
        {
            return std::unexpected(
                httpError(400, std::string("Invalid ") + key));
        }
        if(std::string_view(key) == "count")
        {
            query.limit = static_cast<int>(std::min<int64_t>(*number, 1000));
        }
        else if(std::string_view(key) == "min_ts")
        {
            // Accepted listens always fit below this saturated lower bound.
            query.range.from = *number == std::numeric_limits<int64_t>::max()
                ? *number : *number + 1;
        }
        else
        {
            query.range.to = number;
        }
    }
    return query;
}

// Pano requests a recording MSID on every now-playing submission.
E<std::string> recordingId(const MusicTrack& track)
{
    Json identity = {"NSWeekly recording v1", track.artist, track.title};
    if(track.release) { identity.push_back(*track.release); }
    ASSIGN_OR_RETURN(auto hex, musicDigest(identity.dump()));
    // UUIDv8 places app-defined digest bits around the version and variant.
    hex[12] = '8';
    const auto nibble = hex[16] <= '9' ? hex[16] - '0' : hex[16] - 'a' + 10;
    hex[16] = "89ab"[nibble & 3];
    return std::format("{}-{}-{}-{}-{}", hex.substr(0, 8), hex.substr(8, 4),
                       hex.substr(12, 4), hex.substr(16, 4), hex.substr(20, 12));
}

E<Json> wireTrack(const MusicTrack& track, bool include_additional_info)
{
    Json metadata = {{"artist_name", track.artist}, {"track_name", track.title}};
    if(track.release) { metadata["release_name"] = *track.release; }
    if(include_additional_info && track.additional_info)
    {
        auto extra = Json::parse(*track.additional_info, nullptr, false);
        if(!extra.is_object())
        {
            return std::unexpected(runtimeError("Invalid stored metadata"));
        }
        metadata["additional_info"] = std::move(extra);
    }
    return Json{{"track_metadata", std::move(metadata)}};
}
}

E<std::optional<MusicIdentity>>
ListenBrainzApi::authenticate(const Request& req)
{
    if(req.get_header_value_count("Authorization") != 1)
    {
        return std::nullopt;
    }
    const auto header = req.get_header_value("Authorization");
    auto scheme = header.substr(0, 6);
    if(mw::toLower(scheme) != "token ")
    {
        return std::nullopt;
    }
    const auto token = header.substr(6);
    if(!validMusicDeviceToken(token))
    {
        return std::nullopt;
    }
    ASSIGN_OR_RETURN(auto digest, musicDigest(token));
    return music.identity(digest);
}

void ListenBrainzApi::validate(const Request& req, Response& res)
{
    if(!req.has_header("Authorization") ||
       req.get_header_value("Authorization").empty())
    {
        fail(res, 400, "No token was sent");
        return;
    }
    auto device = authenticate(req);
    if(!device) { fail(res, device.error()); return; }
    if(!*device)
    {
        send(res, {{"code", 200}, {"message", "Token invalid."},
                   {"valid", false}});
        return;
    }
    send(res, {{"code", 200}, {"message", "Token valid."}, {"valid", true},
               {"user_name", (*device)->name}});
}

void ListenBrainzApi::submit(const Request& req, Response& res,
                            const httplib::ContentReader& reader)
{
    const auto started = MusicClock::now();
    const auto received = musicEpochSeconds();
    auto device = authenticate(req);
    if(!device) { fail(res, device.error()); return; }
    if(!*device) { fail(res, 401, "Invalid device authorization"); return; }
    if(!musicMediaType(req, "application/json"))
    {
        fail(res, 415, "Content-Type must be application/json");
        return;
    }
    auto body = musicBody(req, res, reader, BODY_LIMIT);
    if(!body) { fail(res, body.error()); return; }
    auto submission = parseSubmission(req, *body, received);
    if(!submission) { fail(res, submission.error()); return; }

    Json response = {{"status", "ok"}};
    if(submission->return_msid)
    {
        auto id = recordingId(submission->listens.front().track);
        if(!id) { fail(res, id.error()); return; }
        response["recording_msid"] = *id;
    }
    auto result = submission->type == ListenType::PLAYING_NOW
        ? music.playing(**device, std::move(submission->listens.front().track))
        : music.complete(**device, submission->listens, received,
                         submission->type == ListenType::SINGLE
                         ? std::optional{started} : std::nullopt);
    if(!result) { fail(res, result.error()); return; }
    send(res, response);
}

E<ListenBrainzApi::ReadAccount>
ListenBrainzApi::readAccount(const Request& req, const std::string& name)
{
    ASSIGN_OR_RETURN(auto owner, users.getUserID(name));
    if(!owner)
    {
        return std::unexpected(httpError(404, "User not found"));
    }
    ASSIGN_OR_RETURN(auto device, authenticate(req));
    return ReadAccount{*owner, device && device->owner_id == *owner};
}

void ListenBrainzApi::history(const Request& req, Response& res)
{
    const auto name = req.matches[1].str();
    auto account = readAccount(req, name);
    if(!account) { fail(res, account.error()); return; }
    auto query = readQuery(req);
    if(!query) { fail(res, query.error()); return; }
    auto history = music.history(account->owner_id, *query);
    if(!history) { fail(res, history.error()); return; }
    auto listens = Json::array();
    for(const auto& listen : *history)
    {
        auto item = wireTrack(listen.track, account->include_additional_info);
        if(!item) { fail(res, item.error()); return; }
        (*item)["listened_at"] = listen.listened_at;
        listens.push_back(std::move(*item));
    }
    send(res, {{"payload", {{"count", listens.size()}, {"user_id", name},
                            {"listens", std::move(listens)}}}});
}

void ListenBrainzApi::playingNow(const Request& req, Response& res)
{
    const auto name = req.matches[1].str();
    auto account = readAccount(req, name);
    if(!account) { fail(res, account.error()); return; }
    auto track = music.nowPlaying(account->owner_id);
    if(!track) { fail(res, track.error()); return; }
    auto listens = Json::array();
    if(*track)
    {
        auto item = wireTrack(**track, account->include_additional_info);
        if(!item) { fail(res, item.error()); return; }
        (*item)["playing_now"] = true;
        (*item)["user_name"] = name;
        listens.push_back(std::move(*item));
    }
    send(res, {{"payload", {{"count", listens.size()}, {"user_id", name},
                            {"playing_now", true},
                            {"listens", std::move(listens)}}}});
}

void ListenBrainzApi::registerRoutes(httplib::Server& server)
{
    const std::string base = LISTENBRAINZ_API_PATH;
    server.Get(base + "/validate-token", [this](const auto& req, auto& res)
    {
        validate(req, res);
    });
    server.Post(base + "/submit-listens",
                [this](const auto& req, auto& res, const auto& reader)
    {
        submit(req, res, reader);
    });
    server.Get(base + "/user/([^/]+)/listens",
               [this](const auto& req, auto& res)
    {
        history(req, res);
    });
    server.Get(base + "/user/([^/]+)/playing-now",
               [this](const auto& req, auto& res)
    {
        playingNow(req, res);
    });
    const auto unsupported = [](const Request&, Response& res)
    {
        fail(res, 404, "ListenBrainz endpoint is unsupported");
    };
    server.Get(base + "/.*", unsupported);
    server.Post(base + "/.*",
                [unsupported](const auto& req, auto& res, const auto&)
    {
        unsupported(req, res);
    });
}
