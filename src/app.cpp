#include <filesystem>
#include <spdlog/spdlog.h>

#include "app.hpp"
#include "route_urls.hpp"
#include "url.hpp"

E<std::unique_ptr<App>> App::create(const Configuration& conf,
                                     int& exit_code)
{
    exit_code = 4;
    ASSIGN_OR_RETURN(auto prefix, URL::fromStr(conf.url_prefix));
    auto app = std::unique_ptr<App>(new App(conf));
    exit_code = 1;
    ASSIGN_OR_RETURN(app->auth, AuthOpenIDConnect::create(
        app->config, prefix.appendPath("openid-redirect").str(),
        std::make_unique<HTTPSession>()));
    exit_code = 2;
    auto path = (std::filesystem::path(conf.data_dir) / "data.db").string();
    ASSIGN_OR_RETURN(auto user_db, SQLite::connectFile(path));
    ASSIGN_OR_RETURN(auto weekly_db, SQLite::connectFile(path));
    auto users = std::make_unique<UserDataSqlite>(std::move(user_db));
    auto weeklies = std::make_unique<WeeklyDataSqlite>(std::move(weekly_db));
    DO_OR_RETURN(users->initializeSchema());
    DO_OR_RETURN(weeklies->initializeSchema());
    ASSIGN_OR_RETURN(auto game_db, SQLite::connectFile(path));
    auto games = std::make_unique<GameDataSqlite>(std::move(game_db));
    DO_OR_RETURN(games->initializeSchema());
    if(!users->createUser("mw").has_value())
    {
        spdlog::error("Failed to create user");
    }
    app->users = std::move(users);
    app->weeklies = std::move(weeklies);
    app->games = std::move(games);
    app->compose();
    exit_code = 0;
    return app;
}

E<std::unique_ptr<App>> App::create(
    const Configuration& conf, std::unique_ptr<AuthInterface> auth,
    std::unique_ptr<UserDataInterface> users,
    std::unique_ptr<WeeklyDataInterface> weeklies,
    std::unique_ptr<GameDataInterface> games)
{
    if(!auth || !users || !weeklies || !games)
    {
        return std::unexpected(runtimeError("Missing application dependency"));
    }
    auto app = std::unique_ptr<App>(new App(conf));
    app->auth = std::move(auth);
    app->users = std::move(users);
    app->weeklies = std::move(weeklies);
    app->games = std::move(games);
    app->compose();
    return app;
}

void App::compose()
{
    sessions = std::make_unique<SessionService>(*auth);
    auth_module = std::make_unique<AuthModule>(*auth, *sessions);
    weekly_module = std::make_unique<WeeklyModule>(
        config, *weeklies, *sessions);
    games_module = std::make_unique<GamesModule>(
        config, *games, *users, *sessions);
}

void App::handleIndexWithInvalidSession(Response& res) const
{
    switch(config.guest_index)
    {
    case GuestIndex::USER_WEEKLY:
        ASSIGN_OR_RESPOND_ERROR(
            auto uid, users->getUserID(config.guest_index_user), res);
        if(!uid.has_value())
        {
            res.set_redirect(urlFor("login", ""));
        }
        res.set_redirect(weeklyURL(config.guest_index_user), 301);
        return;
    }
    res.status = 500;
    res.set_content("Someone forgot to add a switch case 🤣", "text/plain");
}

void App::handleIndex(const Request& req, Response& res) const
{
    E<SessionValidation> session = sessions->validateSession(req);
    if(!session.has_value())
    {
        handleIndexWithInvalidSession(res);
        return;
    }

    switch(session->status)
    {
    case SessionValidation::INVALID:
        handleIndexWithInvalidSession(res);
        return;
    case SessionValidation::VALID:
        res.set_redirect(weeklyURL(session->user.name), 302);
        return;
    case SessionValidation::REFRESHED:
        sessions->setTokenCookies(session->new_tokens, res);
        res.set_redirect(weeklyURL(session->user.name), 302);
        return;
    }
}

void App::setup()
{
    auto statics_dir = (std::filesystem::path(config.data_dir) /
                        "statics").string();
    spdlog::info("Mounting static dir at {}...", statics_dir);
    if(!server.set_mount_point("/statics", statics_dir))
    {
        spdlog::error("Failed to mount statics");
    }
    server.Get("/", [this](const Request& req, Response& res)
    {
        handleIndex(req, res);
    });
    auth_module->registerRoutes(server);
    weekly_module->registerRoutes(server);
    games_module->registerRoutes(server);
    spdlog::info("Listening at http://{}:{}/...", config.listen_address,
                 config.listen_port);
}
