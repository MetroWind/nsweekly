#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <filesystem>
#include <charconv>

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <mw/utils.hpp>

#include "error.hpp"

using Clock = std::chrono::system_clock;
using Time = std::chrono::time_point<Clock>;

template <typename Bytes>
nlohmann::json parseJSON(Bytes&& bs)
{
    return nlohmann::json::parse(bs, nullptr, false);
}

inline std::string urlEncode(std::string_view s)
{
    char* url_raw = curl_easy_escape(nullptr, s.data(), s.size());
    std::string url(url_raw);
    curl_free(url_raw);
    return url;
}

inline int64_t timeToSeconds(const Time& t)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        t.time_since_epoch()).count();
}

inline Time secondsToTime(const int64_t t)
{
    return Time(std::chrono::seconds(t));
}

inline int daysSinceNewYear(const Time& t)
{
    std::chrono::year_month_day date(std::chrono::floor<std::chrono::days>(t));
    std::chrono::year_month_day new_year(
        date.year(), std::chrono::January, std::chrono::day(1));
    return std::chrono::floor<std::chrono::days>(t.time_since_epoch()).count() -
        std::chrono::floor<std::chrono::days>(
            std::chrono::sys_days(new_year).time_since_epoch()).count();
}

inline E<Time> strToDate(const std::string& s)
{
    std::tm t;
    std::istringstream ss(s);
    ss >> std::get_time(&t, "%Y-%m-%d");
    if(ss.fail())
    {
        return std::unexpected(runtimeError("Invalid date"));
    }
    std::chrono::year_month_day date(
        std::chrono::year(t.tm_year + 1900),
        std::chrono::month(t.tm_mon + 1),
        std::chrono::day(t.tm_mday));
    if(!date.ok())
    {
        return std::unexpected(runtimeError("Invalid date"));
    }
    return std::chrono::sys_days(date);
}
