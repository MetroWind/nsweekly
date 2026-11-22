#pragma once

#include <inja.hpp>

#include "music_data.hpp"
#include "session_service.hpp"
#include "user_data.hpp"

// Renders public Music history and manages private device credentials.
class MusicModule
{
public:
    // Borrows shared sessions and storage for browser requests.
    MusicModule(const Configuration& config, MusicDataInterface& data,
                UserDataInterface& users, SessionService& sessions,
                const std::chrono::time_zone& zone);
    // Registers the public history and session-authenticated settings routes.
    void registerRoutes(httplib::Server& server);

private:
    struct FormNonce
    {
        std::string owner;
        MusicClock::time_point expires;
    };

    E<std::string> issueNonce(const std::string& owner);
    bool checkNonce(const std::string& owner, const std::string& nonce,
                    bool consume);

    std::optional<std::string> account(const httplib::Request& req,
                                       httplib::Response& res);
    std::optional<std::string> requireAccount(const httplib::Request& req,
                                              httplib::Response& res);
    void history(const httplib::Request& req, httplib::Response& res,
                 const std::string& requested_user);
    void settings(const httplib::Request& req, httplib::Response& res);
    void device(const httplib::Request& req, httplib::Response& res,
                const httplib::ContentReader& reader, bool revoke);
    void render(httplib::Response& res, const char* path,
                const nlohmann::json& context);

    MusicDataInterface& music;
    UserDataInterface& users;
    SessionService& sessions;
    const std::chrono::time_zone& zone;
    const std::string guest_user;
    std::string pano_url;
    inja::Environment templates;
    std::mutex render_mutex;
    std::mutex nonce_mutex;
    std::map<std::string, FormNonce> nonces;
};
