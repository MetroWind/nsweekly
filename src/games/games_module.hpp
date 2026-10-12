#pragma once
#include <inja.hpp>
#include <mutex>
#include <mw/http_server.hpp>
#include "config.hpp"
#include "games/game_data.hpp"
#include "games/game_screenshot_service.hpp"
#include "session_service.hpp"
#include "user_data.hpp"

// Serves public trackers and owner-only native HTML action forms.
class GamesModule
{
  public:
    // Borrows stable application dependencies and owns template state.
    GamesModule(const Configuration &conf, GameDataInterface &games,
                UserDataInterface &users, SessionService &sessions,
                GameScreenshotService* screenshots = nullptr);
    // Keeps registered callback targets at their original address.
    GamesModule(const GamesModule &) = delete;
    GamesModule &operator=(const GamesModule &) = delete;
    GamesModule(GamesModule &&) = delete;
    GamesModule &operator=(GamesModule &&) = delete;
    // Registers games routes once on the application's server.
    void registerRoutes(httplib::Server &server);
    // Handles a public table or an owner action, using route identity.
    void handle(const mw::HTTPServer::Request &req,
                mw::HTTPServer::Response &res, const std::string &user,
                const std::string &action = "",
                const std::string &id_text = "",
                const std::string &entry_text = "");
    // Redirects an authenticated account to its tracker, guests to login.
    void handleIndex(const mw::HTTPServer::Request &req,
                     mw::HTTPServer::Response &res);

  private:
    void handleSession(const mw::HTTPServer::Request &req,
                       mw::HTTPServer::Response &res, const std::string &user,
                       const std::string &action, const std::string &id_text,
                       const E<SessionValidation> &session,
                       const std::string &entry_text);
    void dispatch(const mw::HTTPServer::Request &req,
                  mw::HTTPServer::Response &res,
                  const httplib::ContentReader *reader = nullptr);
    void receive(const mw::HTTPServer::Request &req,
                 mw::HTTPServer::Response &res,
                 const httplib::ContentReader &reader, const std::string &user,
                 const std::string &action, const std::string &id_text,
                 const std::string &entry_text);
    void handleScreenshots(const mw::HTTPServer::Request& req,
        mw::HTTPServer::Response& res, const std::string& user,
        const std::string& action, const std::string& id_text,
        const std::string& shot_text, const httplib::ContentReader* reader);
    void handleJournal(const mw::HTTPServer::Request &req,
                       mw::HTTPServer::Response &res, const std::string &user,
                       const std::string &session_user,
                       const std::string &action, int64_t id, int64_t entry_id);
    void handleReviews(const mw::HTTPServer::Request &req,
                       mw::HTTPServer::Response &res, const std::string &user,
                       const std::string &session_user,
                       const std::string &action, int64_t id);
    void render(mw::HTTPServer::Response &res, const std::string &user,
                const std::string &session_user, const std::string &action,
                int64_t id, const GameFields &fields,
                const std::map<std::string, std::string> &errors);
    const Configuration &config;
    GameDataInterface &games;
    UserDataInterface &users;
    SessionService &sessions;
    GameScreenshotService* screenshots;
    inja::Environment templates;
    std::mutex template_mutex;
};
