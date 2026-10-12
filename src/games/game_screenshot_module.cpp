#include "games/games_module.hpp"
#include "games/game.hpp"
#include "http_adaptation.hpp"
#include "open_graph.hpp"
#include "route_urls.hpp"
#include <charconv>
#include <spdlog/spdlog.h>

namespace
{
using Json = nlohmann::json;
using Request = mw::HTTPServer::Request;
using Response = mw::HTTPServer::Response;

E<int64_t> positiveID(std::string_view text)
{
    int64_t id = 0;
    auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), id);
    if(code != std::errc{} || end != text.data() + text.size() || id <= 0)
    { return std::unexpected(httpError(400, "Invalid identity")); }
    return id;
}

Json screenshotError(const Error& error, int& status)
{
    status = 500;
    if(auto http = error.as<HTTPError>()) { status = http->code; }
    std::string code = "storage_error", message = "Unable to save screenshot";
    switch(status)
    {
    case 400: code = "invalid_request"; message = "Malformed screenshot request"; break;
    case 401: code = "authentication_required"; message = "Please log in"; break;
    case 403: code = "forbidden"; message = "Screenshot management is owner-only and requires a matching Origin"; break;
    case 404: code = "not_found"; message = "Game or screenshot not found"; break;
    case 409: code = "attachment_conflict"; message = "Attachment belongs to another gallery or is awaiting cleanup"; break;
    case 413: code = "upload_limit"; message = "File exceeds the upload limit"; break;
    case 415: code = "unsupported_media_type"; message = "Expected multipart form data"; break;
    case 422: code = "invalid_image"; message = "Invalid filename, unsupported image, or color information cannot be preserved"; break;
    default: break;
    }
    spdlog::warn("Screenshot request: {}", errorMsg(error));
    return {{"error", code}, {"message", message}};
}

Json variantJSON(const ImageVariant& variant)
{
    return {{"url", variant.public_url},
            {"width", variant.width}, {"height", variant.height}};
}

Json screenshotJSON(const GameScreenshot& shot)
{
    auto original = attachmentURL(shot.attachment.file.hash,
                                  shot.attachment.original_name);
    return {{"id", shot.id}, {"already_uploaded", shot.already_uploaded},
            {"filename", shot.attachment.original_name},
            {"thumbnail", variantJSON(shot.thumbnail)},
            {"preview", variantJSON(shot.preview)},
            {"original_url", original ? *original : ""}};
}
}

void GamesModule::handleScreenshots(const Request& req, Response& res,
    const std::string& user, const std::string& action,
    const std::string& id_text, const std::string& shot_text,
    const httplib::ContentReader* reader)
{
    res.set_header("Cache-Control", "no-store");
    const bool json = req.get_header_value("Accept").find("application/json") !=
        std::string::npos;
    auto fail = [&res, json](const Error& error)
    {
        int status;
        auto result = screenshotError(error, status);
        res.status = status;
        if(json) { res.set_content(result.dump(), "application/json"); }
        else
        {
            res.set_content("<!doctype html><title>Screenshot error</title><p>" +
                gameEscape(result["message"].get<std::string>()) + "</p>",
                "text/html; charset=utf-8");
        }
    };
    if(!screenshots) { fail(httpError(404, "Unavailable")); return; }
    auto session = sessions.validateSession(req);
    std::string session_user;
    if(session && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
        if(session->status == SessionValidation::REFRESHED)
        { sessions.setTokenCookies(session->new_tokens, res); }
    }
    const bool owner = !session_user.empty() && session_user == user;
    const bool management = action != "screenshots";
    if(management && !owner)
    { fail(httpError(session_user.empty() ? 401 : 403, "Owner required")); return; }
    auto id = positiveID(id_text);
    if(!id) { fail(id.error()); return; }
    if(req.target.find('?') != req.target.npos)
    { fail(httpError(400, "Unexpected query")); return; }
    if(req.method == "POST")
    {
        const auto origin = req.get_header_value("Origin");
        if(req.get_header_value_count("Origin") != 1 || origin.empty() ||
           origin != httpOrigin(origin) ||
           origin != httpOrigin(config.url_prefix))
        { fail(httpError(403, "Invalid Origin")); return; }
    }
    if((action == "screenshots" && req.method != "GET") ||
       (action == "screenshots/upload" && req.method != "POST"))
    { fail(httpError(404, "Unknown action")); return; }
    auto game = games.getGame(user, *id);
    if(!game) { fail(game.error()); return; }
    if(!*game) { fail(httpError(404, "Unknown game")); return; }
    auto uid = users.getUserID(user);
    if(!uid) { fail(uid.error()); return; }
    if(!*uid) { fail(httpError(404, "Unknown owner")); return; }
    const auto base = gamesURL(user) + "/" + std::to_string(*id);
    const auto gallery = base + "/screenshots";
    Json context{
        {"table_url", gameEscape(gamesURL(user))},
        {"weekly_url", gameEscape(weeklyURL(user))},
        {"username", gameEscape(user)},
        {"session_user", gameEscape(session_user)}, {"is_owner", owner},
        {"attachments_url", gameEscape(urlFor("attachments", session_user))},
        {"game_name", gameEscape((*game)->input.name)},
        {"game_url", gameEscape(base)}, {"gallery_url", gameEscape(gallery)},
        {"upload_url", gameEscape(gallery + "/upload")},
        {"deletion", false}, {"batch_results", false},
        {"shots", Json::array()}, {"results", Json::array()},
        {"open_graph", openGraph(config.url_prefix,
            (*game)->input.name + " screenshots", gallery,
            "Public game screenshots by " + user + ".")}};
    if(action == "screenshots/upload")
    {
        if(!reader || !req.is_multipart_form_data())
        { fail(httpError(415, "Expected multipart")); return; }
        std::optional<ScreenshotWriter> writer;
        std::optional<Error> part_error;
        std::string filename;
        bool active = false;
        size_t parts = 0;
        auto results = Json::array();
        int last_status = 200;
        auto completed = [&]
        {
            if(!active) { return; }
            Json result;
            if(!part_error && writer)
            {
                auto saved = writer->finish();
                if(saved) { result = screenshotJSON(*saved); last_status = 200; }
                else { part_error = saved.error(); }
            }
            if(part_error)
            {
                result = screenshotError(*part_error, last_status);
                result["filename"] = validGameText(filename) ? filename :
                    "Invalid filename";
            }
            result["success"] = !part_error;
            results.push_back(std::move(result));
            writer.reset();
            part_error.reset();
            active = false;
        };
        bool read = (*reader)(
            [&](const httplib::FormData& part)
            {
                if(json && active)
                {
                    // JSON is exactly one file: do not publish a prior part
                    // before the complete request structure is validated.
                    writer.reset();
                    part_error.reset();
                    active = false;
                }
                else { completed(); }
                active = true;
                filename = part.filename;
                ++parts;
                if(part.name != (json ? "file" : "files") ||
                   filename.empty() || (json && parts != 1))
                { part_error = httpError(400, "Invalid multipart fields"); }
                else
                {
                    auto upload = screenshots->beginUpload(user, **uid, *id,
                                                           filename);
                    if(upload) { writer.emplace(std::move(*upload)); }
                    else { part_error = upload.error(); }
                }
                return true;
            },
            [&](const char* bytes, size_t size)
            {
                if(writer && !part_error)
                {
                    auto appended = writer->append({bytes, size});
                    if(!appended) { part_error = appended.error(); writer.reset(); }
                }
                return true;
            });
        if(!read)
        {
            writer.reset();
            part_error = httpError(400, "Interrupted or corrupt multipart");
            // A structurally incomplete part is never published.
            if(!active) { active = true; filename = "Unprocessed remainder"; }
        }
        if(json && parts != 1)
        {
            writer.reset();
            part_error = httpError(400, "Expected exactly one file");
        }
        completed();
        if(parts == 0 && read) { fail(httpError(400, "No files selected")); return; }
        if(json)
        {
            if(parts != 1 || !read)
            { fail(httpError(400, "Invalid multipart batch")); return; }
            res.status = last_status;
            auto result = results[0];
            result.erase("success");
            res.set_content(result.dump(), "application/json");
            return;
        }
        context["batch_results"] = true;
        for(auto& result : results)
        {
            result["filename"] = gameEscape(result["filename"].get<std::string>());
            if(!result["success"].get<bool>())
            { result["message"] = gameEscape(result["message"].get<std::string>()); }
        }
        context["results"] = results;
    }
    else
    {
        auto records = screenshots->list(*id);
        if(!records) { fail(records.error()); return; }
        int64_t shot_id = 0;
        if(action == "screenshots/delete")
        {
            auto parsed = positiveID(shot_text);
            if(!parsed) { fail(parsed.error()); return; }
            shot_id = *parsed;
            auto found = std::ranges::find(*records, shot_id, &GameScreenshot::id);
            if(found == records->end())
            { fail(httpError(404, "Unknown screenshot")); return; }
            if(req.method == "POST")
            {
                if(httplib::detail::extract_media_type(
                    req.get_header_value("Content-Type")) !=
                    "application/x-www-form-urlencoded")
                { fail(httpError(415, "Expected form")); return; }
                bool nonempty = !req.body.empty();
                if(reader && !(*reader)([&](const char*, size_t size)
                    { nonempty = nonempty || size != 0; return !nonempty; }))
                { fail(httpError(400, "Unexpected body")); return; }
                if(nonempty) { fail(httpError(400, "Unexpected fields")); return; }
                auto removed = screenshots->remove(**uid, *id, shot_id);
                if(!removed) { fail(removed.error()); return; }
                res.set_redirect(gallery, 303);
                return;
            }
            context["deletion"] = true;
            context["action_url"] = gameEscape(gallery + "/" +
                std::to_string(shot_id) + "/delete");
        }
        for(const auto& record : *records)
        {
            if(shot_id && shot_id != record.id) { continue; }
            auto shot = screenshotJSON(record);
            shot["filename"] = gameEscape(record.attachment.original_name);
            shot["original_url"] = gameEscape(shot["original_url"].get<std::string>());
            shot["thumbnail"]["url"] = gameEscape(record.thumbnail.public_url);
            shot["preview"]["url"] = gameEscape(record.preview.public_url);
            shot["delete_url"] = gameEscape(gallery + "/" +
                std::to_string(record.id) + "/delete");
            context["shots"].push_back(std::move(shot));
        }
    }
    try
    {
        std::lock_guard lock(template_mutex);
        res.set_content(templates.render_file("games/screenshots.html", context),
                        "text/html; charset=utf-8");
    }
    catch(const inja::InjaError& error)
    { fail(runtimeError(error.what())); }
}
