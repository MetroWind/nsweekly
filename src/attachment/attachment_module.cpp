#include "attachment/attachment_module.hpp"
#include "games/game.hpp"
#include "http_adaptation.hpp"
#include "open_graph.hpp"
#include "route_urls.hpp"
#include <charconv>
#include <spdlog/spdlog.h>
#include <sys/stat.h>

namespace
{
void failure(httplib::Response& res, const Error& error)
{
    const auto* http = error.as<HTTPError>();
    res.status = http ? http->code : 500;
    if(!http)
    {
        spdlog::error("Attachment operation: {}", errorMsg(error));
    }
    res.set_header("Cache-Control", "no-store");
    const std::string reason =
        res.status == 413   ? "Upload exceeds the byte limit. "
        : res.status == 415 ? "Unsupported request media type. "
        : res.status == 422 ? "Invalid filename. "
        : res.status == 401 ? "Login required. "
        : res.status == 403 ? "This request is not authorized. "
        : res.status == 404 ? "Attachment or route not found. "
        : res.status == 400 ? "Malformed request. "
                            : "Storage is unavailable. ";
    res.set_content(
        "<!doctype html><meta charset=\"utf-8\">"
        "<title>Attachment request failed</title><h1>Attachment request "
        "failed</h1>"
        "<p>" +
            reason +
            "Check the request and try again. Filenames must be valid UTF-8, "
            "1–1024 bytes, without control characters. "
            "For uploads, choose the file again.</p>",
        "text/html; charset=utf-8");
}
void uploadFailure(httplib::Response &response, const Error &error,
                   bool json_mode, const std::string &override_code = "")
{
    failure(response, error);
    if(!json_mode)
    {
        return;
    }
    std::string code = "upload_failed";
    if(response.status == 401) code = "login_required";
    else if(response.status == 403) code = "owner_required";
    else if(response.status == 400 || response.status == 422)
        code = "invalid_upload";
    else if(response.status == 415) code = "unsupported_media_type";
    else if(response.status == 413) code = "upload_too_large";
    if(!override_code.empty()) code = override_code;
    nlohmann::json body{{"error", {{"code", code},
        {"message", response.status == 413 ?
            "The file exceeds the upload limit." :
            "Upload failed. Check the request and try again."}}}};
    response.set_content(body.dump(), "application/json");
}

std::string managerURL(const std::string& user)
{
    return "/attachments/" + attachmentSegment(user);
}
} // namespace

AttachmentModule::AttachmentModule(const Configuration& conf,
                                   AttachmentService& storage,
                                   SessionService& session_service,
                                   UserDataInterface& user_storage)
    : config(conf), service(storage), sessions(session_service),
      users(user_storage),
      templates(
          (std::filesystem::path(conf.data_dir) / "templates" / "").string())
{
    templates.add_callback("url_for", 2,
                           [](const inja::Arguments& args)
                           {
                               return gameEscape(
                                   urlFor(args[0]->get<std::string>(),
                                          args[1]->get<std::string>()));
                           });
}
void AttachmentModule::registerRoutes(httplib::Server& server)
{
    server.Get("/attachments",
               [](const auto&, auto& res)
               {
                   res.set_redirect("/attachments/", 308);
               });
    server.Get("/attachments/.*",
               [this](const auto& req, auto& res)
               {
                   handle(req, res);
               });
    server.Post(
        "/attachments/.*",
        [this](const auto& req, auto& res, const httplib::ContentReader& reader)
        {
            handle(req, res, &reader);
        });
    server.Get("/attachment/.*",
               [this](const auto& req, auto& res)
               {
                   download(req, res);
               });
}
void AttachmentModule::handle(const httplib::Request& req,
                              httplib::Response& res,
                              const httplib::ContentReader* reader)
{
    const bool json_mode = req.get_header_value("Accept") == "application/json";
    res.set_header("Cache-Control", "no-store");
    auto session = sessions.validateSession(req);
    if(session && session->status == SessionValidation::REFRESHED)
    {
        sessions.setTokenCookies(session->new_tokens, res);
    }
    const auto path = req.target.substr(0, req.target.find('?'));
    bool index = path == "/attachments/";
    if(!session || session->status == SessionValidation::INVALID)
    {
        if(index && !reader)
        {
            res.set_redirect("/login", 302);
        }
        else
        {
            uploadFailure(res, httpError(401, "Login required"), json_mode);
        }
        return;
    }
    if(index && !reader)
    {
        res.set_redirect(managerURL(session->user.name), 302);
        return;
    }
    auto suffix = path.substr(13);
    auto slash = suffix.find('/');
    auto user = httplib::decode_path_component(suffix.substr(0, slash));
    if(user.empty() || !validGameText(user))
    {
        uploadFailure(res, httpError(400, "Invalid user"), json_mode);
        return;
    }
    if(user != session->user.name)
    {
        uploadFailure(res, httpError(403, "Owner required"), json_mode);
        return;
    }
    auto action = slash == suffix.npos ? "" : suffix.substr(slash + 1);
    int64_t id = 0;
    bool deletion = false;
    if(!action.empty() && action != "upload")
    {
        auto end = action.find('/');
        auto text = action.substr(0, end);
        auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), id);
        if(end == action.npos || action.substr(end) != "/delete")
        {
            uploadFailure(res, httpError(404, "Unknown route"), json_mode);
            return;
        }
        if(parsed.ec != std::errc() ||
           parsed.ptr != text.data() + text.size() || id < 1)
        {
            uploadFailure(res, httpError(400, "Invalid record ID"), json_mode);
            return;
        }
        deletion = true;
    }
    if((reader && action.empty()) || (!reader && action == "upload"))
    {
        uploadFailure(res, httpError(404, "Unknown route"), json_mode);
        return;
    }
    if(reader)
    {
        auto supplied = req.get_header_value("Origin");
        if(req.get_header_value_count("Origin") != 1 || supplied.empty() ||
           supplied != httpOrigin(supplied) ||
           httpOrigin(supplied) != httpOrigin(config.url_prefix))
        {
            uploadFailure(res, httpError(403, "Origin required"), json_mode,
                "origin_required");
            return;
        }
    }
    auto ensured = users.ensureUser(user);
    if(!ensured)
    {
        uploadFailure(res, ensured.error(), json_mode);
        return;
    }
    auto owner = users.getUserID(user);
    if(!owner)
    {
        uploadFailure(res, owner.error(), json_mode);
        return;
    }
    if(!*owner)
    {
        uploadFailure(res, httpError(404, "Unknown owner"), json_mode);
        return;
    }
    auto manager = managerURL(user);
    if(reader && action == "upload")
    {
        if(httplib::detail::extract_media_type(
               req.get_header_value("Content-Type")) != "multipart/form-data")
        {
            uploadFailure(res, httpError(415, "Multipart required"), json_mode);
            return;
        }
        std::optional<AttachmentWriter> writer;
        std::optional<Error> error;
        size_t parts = 0;
        bool read = (*reader)(
            [&](const httplib::FormData& part)
            {
                ++parts;
                if(parts != 1 || part.name != "file" || part.filename.empty())
                {
                    error = httpError(400, "Exactly one file required");
                    return false;
                }
                auto upload = service.beginUpload(**owner, part.filename);
                if(!upload)
                {
                    error = upload.error();
                    return false;
                }
                writer.emplace(std::move(*upload));
                return true;
            },
            [&](const char* bytes, size_t size)
            {
                if(!writer)
                {
                    error = httpError(400, "Missing file");
                    return false;
                }
                auto appended = writer->append({bytes, size});
                if(!appended)
                {
                    error = appended.error();
                    return false;
                }
                return true;
            });
        if(error)
        {
            uploadFailure(res, *error, json_mode);
            return;
        }
        if(!read || parts != 1 || !writer)
        {
            uploadFailure(res, httpError(400, "Malformed multipart"), json_mode);
            return;
        }
        auto result = writer->finish();
        if(!result)
        {
            uploadFailure(res, result.error(), json_mode);
            return;
        }
        if(json_mode)
        {
            const auto &record = result->record;
            nlohmann::json body{
                {"id", record.id}, {"filename", record.original_name},
                {"mime_type", record.file.mime_type},
                {"byte_size", record.file.byte_size},
                {"public_url", result->public_url},
                {"already_owned", result->already_owned}};
            res.status = 200;
            res.set_content(body.dump(), "application/json");
            return;
        }
        res.set_redirect(manager + (result->already_owned ? "?status=duplicate"
                                                          : "?status=uploaded"),
                         303);
        return;
    }
    if(deletion)
    {
        auto record = service.getAttachment(**owner, id);
        if(!record)
        {
            uploadFailure(res, record.error(), json_mode);
            return;
        }
        if(reader)
        {
            if(httplib::detail::extract_media_type(req.get_header_value(
                   "Content-Type")) != "application/x-www-form-urlencoded")
            {
                uploadFailure(res, httpError(415, "URL encoded form required"), json_mode);
                return;
            }
            bool nonempty = false;
            bool read = (*reader)(
                [&](const char*, size_t size)
                {
                    nonempty = size != 0;
                    return !nonempty;
                });
            if(!read || nonempty || req.target.find('?') != req.target.npos)
            {
                uploadFailure(res, httpError(400, "Empty form required"), json_mode);
                return;
            }
            auto result = service.deleteAttachment(**owner, id);
            if(!result)
            {
                uploadFailure(res, result.error(), json_mode);
                return;
            }
            res.set_redirect(manager + "?status=deleted", 303);
        }
        else
        {
            render(res, user, manager, {*record}, "", true);
        }
        return;
    }
    auto records = service.listAttachments(**owner);
    if(!records)
    {
        uploadFailure(res, records.error(), json_mode);
        return;
    }
    auto status = req.get_param_value("status");
    std::string notice = status == "uploaded"    ? "Uploaded"
                         : status == "duplicate" ? "Already uploaded"
                         : status == "deleted"   ? "Deleted"
                                                 : "";
    render(res, user, manager, *records, notice, false);
}
void AttachmentModule::render(httplib::Response& res, const std::string& user,
                              const std::string& manager,
                              const std::vector<Attachment>& records,
                              const std::string& notice, bool deletion)
{
    auto rows = nlohmann::json::array();
    for(const auto& record : records)
    {
        auto url = attachmentURL(record.file.hash, record.original_name);
        if(!url)
        {
            failure(res, url.error());
            return;
        }
        auto date =
            std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(
                std::chrono::sys_seconds{std::chrono::seconds{record.added}}));
        auto prefix = config.url_prefix;
        while(!prefix.empty() && prefix.back() == '/')
        {
            prefix.pop_back();
        }
        rows.push_back(
            {{"name", gameEscape(record.original_name)},
             {"size", record.file.byte_size},
             {"date",
              std::format("{:04}-{:02}-{:02}", int(date.year()),
                          unsigned(date.month()), unsigned(date.day()))},
             {"url", gameEscape(prefix + *url)},
             {"delete_url",
              gameEscape(manager + "/" + std::to_string(record.id) +
                         "/delete")}});
    }
    nlohmann::json context{
        {"session_user", gameEscape(user)},
        {"username", gameEscape(user)},
        {"manager_url", gameEscape(manager)},
        {"attachments", rows},
        {"attachment_manager", true},
        {"attachments_url", gameEscape(manager)},
        {"notice", notice},
        {"open_graph", openGraph(config.url_prefix, "Attachments", manager,
                                 "Manage public attachments.")}};
    try
    {
        std::lock_guard lock(template_mutex);
        res.set_content(
            templates.render_file(deletion ? "attachment/delete.html"
                                           : "attachment/attachments.html",
                                  context),
            "text/html; charset=utf-8");
    }
    catch(const std::exception& error)
    {
        failure(res, runtimeError(error.what()));
    }
}
void AttachmentModule::download(const httplib::Request& req,
                                httplib::Response& res)
{
    const auto path = req.target.substr(0, req.target.find('?'));
    auto suffix = path.substr(12);
    auto slash = suffix.find('/');
    if(slash == suffix.npos || suffix.find('/', slash + 1) != suffix.npos)
    {
        failure(res, httpError(404, "Missing public file"));
        return;
    }
    auto hash = suffix.substr(0, slash);
    auto name = httplib::decode_path_component(suffix.substr(slash + 1));
    auto validated = attachmentName(name);
    if(!validated || *validated != name || !attachmentHash(hash))
    {
        failure(res, httpError(400, "Invalid public URL"));
        return;
    }
    auto file = attachmentPath(service.directory(), hash);
    if(!file)
    {
        failure(res, file.error());
        return;
    }
    struct stat info{};
    if(lstat(file->c_str(), &info) != 0 || !S_ISREG(info.st_mode))
    {
        failure(res, httpError(404, "Public file missing"));
        return;
    }
    auto mime = probeMimeType(*file);
    bool inline_image =
        mime == "image/jpeg" || mime == "image/png" || mime == "image/webp";
    std::string ascii;
    for(unsigned char c : name)
    {
        if(c >= 32 && c < 127 && c != '"' && c != '\\' && c != '%')
        {
            ascii += c;
        }
        else
        {
            ascii += '_';
        }
    }
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Cache-Control", "public, max-age=3600");
    res.set_header("ETag", "\"" + hash + "\"");
    res.set_header("Content-Disposition",
                   std::string(inline_image ? "inline" : "attachment") +
                       "; filename=\"" + ascii + "\"; filename*=UTF-8''" +
                       attachmentSegment(name));
    if(req.get_header_value("If-None-Match") == "\"" + hash + "\"")
    {
        res.status = 304;
        return;
    }
    res.set_file_content(file->string(),
                         inline_image ? mime : "application/octet-stream");
}
