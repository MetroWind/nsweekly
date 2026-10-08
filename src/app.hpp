#pragma once
#include <memory>
#include <mw/http_server.hpp>
#include "auth_module.hpp"
#include "config.hpp"
#include "user_data.hpp"
#include "weekly_module.hpp"

// Owns dependencies and composes the service at a stable address.
class App : public mw::HTTPServer
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
    void handleIndex(const Request& req, Response& res) const;
    // Mounts statics and registers root and module routes.
    void registerRoutes(httplib::Server& server);
private:
    explicit App(const Configuration& conf)
        : mw::HTTPServer(mw::IPSocketInfo{
              conf.listen_address, conf.listen_port}), config(conf)
    {}
    void setup() override;
    void compose();
    void handleIndexWithInvalidSession(Response& res) const;
    const Configuration config;
    std::unique_ptr<AuthInterface> auth;
    std::unique_ptr<UserDataInterface> users;
    std::unique_ptr<WeeklyDataInterface> weeklies;
    std::unique_ptr<SessionService> sessions;
    std::unique_ptr<AuthModule> auth_module;
    std::unique_ptr<WeeklyModule> weekly_module;
};
