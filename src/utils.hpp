#pragma once

#include <nlohmann/json.hpp>

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
