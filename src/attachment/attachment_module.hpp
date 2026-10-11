#pragma once
#include "attachment/attachment_service.hpp"
#include "session_service.hpp"
#include "user_data.hpp"
#include <inja.hpp>

// Streams owner uploads and exposes public immutable downloads.
class AttachmentModule
{
public:
    // Borrows application-owned storage, session and user services.
    AttachmentModule(const Configuration& conf, AttachmentService& service,
                     SessionService& sessions, UserDataInterface& users);
    // Installs plural management and singular public routes.
    void registerRoutes(httplib::Server& server);
    // Handles an owner route; reader is present for streaming POST bodies.
    void handle(const httplib::Request& req, httplib::Response& res,
                const httplib::ContentReader* reader = nullptr);
    // Serves bytes with no authentication or database dependency.
    void download(const httplib::Request& req, httplib::Response& res);

private:
    void render(httplib::Response& res, const std::string& user,
                const std::string& manager,
                const std::vector<Attachment>& records,
                const std::string& notice, bool deletion);
    const Configuration& config;
    AttachmentService& service;
    SessionService& sessions;
    UserDataInterface& users;
    inja::Environment templates;
    std::mutex template_mutex;
};
