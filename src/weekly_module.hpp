#pragma once
#include <inja.hpp>
#include <httplib.h>
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
    void handleUserWeeklies(const httplib::Request& req, httplib::Response& res,
                            const std::string& username);
    // Renders an individual weekly or returns 404 for an empty range.
    void handleUserWeekly(const httplib::Request& req, httplib::Response& res,
                          const std::string& username, const Time& date);
    // Renders raw edit content after checking identity and Monday date.
    void handleEditFrontEnd(const httplib::Request& req, httplib::Response& res,
                            const std::string& username,
                            const Time& week_start);
    // Saves an authorized Monday post and redirects to the root.
    void handleEdit(const httplib::Request& req, httplib::Response& res,
                    const std::string& username, const Time& week_start) const;
private:
    void handleWeeklyRoute(const httplib::Request&, httplib::Response&);
    void handleEditPageRoute(const httplib::Request&, httplib::Response&);
    void handleEditRoute(const httplib::Request&, httplib::Response&);
    const Configuration& config;
    WeeklyDataInterface& data;
    SessionService& sessions;
    inja::Environment templates;
};
