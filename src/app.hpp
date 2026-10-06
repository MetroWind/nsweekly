#pragma once
#include <memory>
#include <httplib.h>
#include "auth_module.hpp"
#include "config.hpp"
#include "user_data.hpp"
#include "weekly_module.hpp"

// Owns dependencies and composes the service at a stable address.
class App
{
public:
    // Keeps route callback targets at a stable address.
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
    // Creates production dependencies; reports the existing startup category.
    static E<std::unique_ptr<App>> create(const Configuration& conf,
                                          int& exit_code);
    // Injects backends without opening files or contacting a provider.
    static E<std::unique_ptr<App>> create(
        const Configuration& conf, std::unique_ptr<AuthInterface> auth,
        std::unique_ptr<UserDataInterface> users,
        std::unique_ptr<WeeklyDataInterface> weeklies);
    // Applies the existing session and guest landing-page policy.
    void handleIndex(const httplib::Request& req,
                     httplib::Response& res) const;
    // Mounts statics and registers root and module routes.
    void registerRoutes(httplib::Server& server);
    // Listens synchronously with a local server.
    void start();
private:
    explicit App(const Configuration& conf) : config(conf) {}
    void compose();
    void handleIndexWithInvalidSession(httplib::Response& res) const;
    const Configuration config;
    std::unique_ptr<AuthInterface> auth;
    std::unique_ptr<UserDataInterface> users;
    std::unique_ptr<WeeklyDataInterface> weeklies;
    std::unique_ptr<SessionService> sessions;
    std::unique_ptr<AuthModule> auth_module;
    std::unique_ptr<WeeklyModule> weekly_module;
};
