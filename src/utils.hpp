#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <filesystem>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include "error.hpp"

#define _CONCAT_NAMES_INNER(a, b) a##b
#define _CONCAT_NAMES(a, b) _CONCAT_NAMES_INNER(a, b)
#define _ASSIGN_OR_RETURN_INNER(tmp, var, val)  \
    auto tmp = val;                             \
    if(!tmp.has_value()) {                      \
        return std::unexpected(tmp.error());    \
    }                                           \
    var = std::move(tmp).value()

// Val should be a rvalue.
#define ASSIGN_OR_RETURN(var, val)                                      \
    _ASSIGN_OR_RETURN_INNER(_CONCAT_NAMES(assign_or_return_tmp, __COUNTER__), var, val)

// Val should be a rvalue.
#define DO_OR_RETURN(val)                               \
    if(auto rt = val; !rt.has_value())                  \
    {                                                   \
        return std::unexpected(std::move(rt).error());  \
    }

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
