#pragma once
#include <inja.hpp>
#include <mw/http_server.hpp>
#include "config.hpp"
#include "weekly_data.hpp"
#include "session_service.hpp"

// Owns weekly HTTP routes and template rendering.
class WeeklyModule
{
public:
    // Borrows stable configuration, storage, and shared sessions.
    WeeklyModule(const Configuration& conf, WeeklyDataInterface& storage,
                 SessionService& session_service);
    // Keeps route callback targets at a stable address.
    WeeklyModule(const WeeklyModule&) = delete;
    WeeklyModule& operator=(const WeeklyModule&) = delete;
    WeeklyModule(WeeklyModule&&) = delete;
    WeeklyModule& operator=(WeeklyModule&&) = delete;
    // Installs the weekly and edit routes exactly once before listening.
    void registerRoutes(httplib::Server& server);
    // Renders the rolling weekly list in newest-first order.
    void handleUserWeeklies(const mw::HTTPServer::Request& req,
                           mw::HTTPServer::Response& res,
                           const std::string& username);
    // Renders an individual weekly or returns 404 for an empty range.
    void handleUserWeekly(const mw::HTTPServer::Request& req,
                         mw::HTTPServer::Response& res,
                         const std::string& username, const Time& date);
    // Renders raw edit content after checking identity and Monday date.
    void handleEditFrontEnd(const mw::HTTPServer::Request& req,
                           mw::HTTPServer::Response& res,
                           const std::string& username,
                           const Time& week_start);
    // Saves an authorized Monday post and redirects to the root.
    void handleEdit(const mw::HTTPServer::Request& req,
                    mw::HTTPServer::Response& res,
                    const std::string& username, const Time& week_start) const;
private:
    void handleWeeklyRoute(const mw::HTTPServer::Request&,
                           mw::HTTPServer::Response&);
    void handleEditPageRoute(const mw::HTTPServer::Request&,
                             mw::HTTPServer::Response&);
    void handleEditRoute(const mw::HTTPServer::Request&,
                         mw::HTTPServer::Response&);
    const Configuration& config;
    WeeklyDataInterface& data;
    SessionService& sessions;
    inja::Environment templates;
};
