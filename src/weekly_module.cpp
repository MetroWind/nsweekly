#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

#include "weekly_module.hpp"
#include "route_urls.hpp"
#include <mw/http_server.hpp>

namespace
{
nlohmann::json weeklyToJSON(const WeeklyPost& p, bool render=true)
{
    std::string content;
    if(render)
    {
        content = *p.render().or_else(
            [](const auto& e) -> E<std::string>
            {
                return errorMsg(e);
            });
    }
    else
    {
        content = p.raw_content;
    }

    auto week_begin_day = std::chrono::floor<std::chrono::days>(p.week_begin);
    std::chrono::year_month_day date(week_begin_day);
    int week = daysSinceNewYear(p.week_begin);
    std::string week_str = std::format("{} week {}", date.year(), week / 7 + 1);
    std::ostringstream ss;
    ss << date;
    return {{ "week_str", week_str },
            { "date_str", ss.str() },
            { "week_begin", std::format("{}", week_begin_day) },
            { "week_end", std::format("{}", week_begin_day +
                                      std::chrono::days(6)) },
            { "update", std::format("{}",
                std::chrono::floor<std::chrono::seconds>(p.update_time)) },
            { "content", std::move(content) },
            { "lang", p.language },
            { "author", p.author },
    };
}

} // namespace

WeeklyModule::WeeklyModule(const Configuration& conf,
                           WeeklyDataInterface& storage,
                           SessionService& session_service)
    : config(conf), data(storage), sessions(session_service),
      templates((std::filesystem::path(config.data_dir) / "templates" / "")
                .string())
{
    templates.add_callback("url_for", 2, [](const inja::Arguments& args)
    {
        return urlFor(args.at(0)->get_ref<const std::string&>(),
                      args.at(1)->get_ref<const std::string&>());
    });
}

void WeeklyModule::handleUserWeeklies(
    const httplib::Request& req, httplib::Response& res,
    const std::string& username)
{
    E<SessionValidation> session = sessions.validateSession(req);
    std::string session_user;
    if(session.has_value() && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
    }

    ASSIGN_OR_RESPOND_ERROR(
        std::vector<WeeklyPost> weeklies,
        data.getWeekliesOneYear(username), res);
    std::reverse(std::begin(weeklies), std::end(weeklies));
    nlohmann::json weeklies_json(nlohmann::json::value_t::array);
    for(const WeeklyPost& p: weeklies)
    {
        weeklies_json.push_back(weeklyToJSON(p));
    }
    nlohmann::json data{{ "weeklies", std::move(weeklies_json) },
                        { "username", username },
                        { "session_user", session_user },
                        { "this_url", req.target },
    };
    std::string result = templates.render_file(
        "weeklies.html", std::move(data));
    res.set_content(result, "text/html");
}

void WeeklyModule::handleUserWeekly(
    const httplib::Request& req, httplib::Response& res,
    const std::string& username, const Time& date)
{
    E<SessionValidation> session = sessions.validateSession(req);
    std::string session_user;
    if(session.has_value() && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
    }

    ASSIGN_OR_RESPOND_ERROR(
        std::vector<WeeklyPost> weeklies,
        data.getWeeklies(username, date, date + std::chrono::days(1)),
        res);
    if(weeklies.empty())
    {
        res.status = 404;
        return;
    }

    nlohmann::json weekly_json = weeklyToJSON(std::move(weeklies)[0]);
    nlohmann::json data{{ "weekly", std::move(weekly_json) },
                        { "username", username },
                        { "session_user", session_user },
                        { "this_url", req.target },
    };
    std::string result = templates.render_file("weekly.html", std::move(data));
    res.set_content(result, "text/html");
}

void WeeklyModule::handleEditFrontEnd(
    const httplib::Request& req, httplib::Response& res,
    const std::string& username, const Time& week_start)
{
    E<SessionValidation> session = sessions.validateSession(req);
    std::string session_user;
    if(session.has_value() && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
    }
    else
    {
        res.status = 401;
        return;
    }

    if(session_user != username)
    {
        res.status = 401;
        return;
    }

    if(std::chrono::weekday(std::chrono::floor<std::chrono::days>(week_start))
       != std::chrono::Monday)
    {
        res.status = 404;
        return;
    }

    ASSIGN_OR_RESPOND_ERROR(
        std::vector<WeeklyPost> weekly, data.getWeeklies(
            username, week_start, week_start + std::chrono::days(1)), res);
    if(weekly.empty())
    {
        res.status = 404;
        return;
    }
    nlohmann::json data{{"weekly", weeklyToJSON(std::move(weekly)[0], false)},
                        {"session_user", session_user}};
    std::string html = templates.render_file("edit.html", std::move(data));
    res.set_content(html, "text/html");
}

void WeeklyModule::handleEdit(
    const httplib::Request& req, httplib::Response& res,
    const std::string& username, const Time& week_start) const
{
    E<SessionValidation> session = sessions.validateSession(req);
    std::string session_user;
    if(session.has_value() && session->status != SessionValidation::INVALID)
    {
        session_user = session->user.name;
    }
    else
    {
        res.status = 401;
        return;
    }

    if(session_user != username)
    {
        res.status = 401;
        return;
    }

    if(std::chrono::weekday(std::chrono::floor<std::chrono::days>(week_start))
       != std::chrono::Monday)
    {
        res.status = 404;
        return;
    }

    WeeklyPost p;
    p.author = username;
    p.format = WeeklyPost::MARKDOWN;
    p.language = config.default_lang;
    p.week_begin = week_start;
    p.raw_content = req.get_param_value("content");
    if(auto r = data.updateWeekly(username, std::move(p));
       !r.has_value())
    {
        res.status = 500;
        res.set_content(errorMsg(r.error()), "text/plain");
        return;
    }
    res.set_redirect(urlFor("index", ""));
}

void WeeklyModule::registerRoutes(httplib::Server& server)
{
    server.Get("/weekly/:username", [this](const httplib::Request& req,
                                          httplib::Response& res)
    {
        handleUserWeeklies(req, res, req.path_params.at("username"));
    });
    server.Get("/weekly/:username/:date", [this](const auto& req, auto& res)
    {
        handleWeeklyRoute(req, res);
    });
    server.Get("/edit/:username/:date", [this](const auto& req, auto& res)
    {
        handleEditPageRoute(req, res);
    });
    server.Post("/edit/:username/:date", [this](const auto& req, auto& res)
    {
        handleEditRoute(req, res);
    });
}

void WeeklyModule::handleWeeklyRoute(const httplib::Request& req,
                                httplib::Response& res)
{
    E<Time> date = strToDate(req.path_params.at("date"));
    if(!date.has_value())
    {
        res.status = 400;
        res.set_content(errorMsg(date.error()), "text/plain");
        return;
    }
    handleUserWeekly(req, res, req.path_params.at("username"), *date);
}

void WeeklyModule::handleEditPageRoute(const httplib::Request& req,
                                httplib::Response& res)
{
    E<Time> date = strToDate(req.path_params.at("date"));
    if(!date.has_value())
    {
        res.status = 400;
        res.set_content(errorMsg(date.error()), "text/plain");
        return;
    }
    handleEditFrontEnd(req, res, req.path_params.at("username"), *date);
}

void WeeklyModule::handleEditRoute(const httplib::Request& req,
                                httplib::Response& res)
{
    std::tm t;
    std::istringstream ss(req.path_params.at("date"));
    ss >> std::get_time(&t, "%Y-%m-%d");
    if(ss.fail())
    {
        res.status = 400;
        res.set_content("Invalid date", "text/plain");
        return;
    }
    std::chrono::year_month_day date(
        std::chrono::year(t.tm_year + 1900),
        std::chrono::month(t.tm_mon + 1),
        std::chrono::day(t.tm_mday));
    if(!date.ok())
    {
        res.status = 400;
        res.set_content("Invalid date", "text/plain");
        return;
    }
    handleEdit(req, res, req.path_params.at("username"),
               std::chrono::sys_days(date));
}
