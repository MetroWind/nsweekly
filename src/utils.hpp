#pragma once

#include <nlohmann/json.hpp>

#define ASSIGN_OR_RETURN(var, val) if(!(val).has_value()) { \
        return std::unexpected((val).error());              \
        }                                                   \
    var = (val).value()

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

inline std::string bytesToString(const std::vector<std::byte> data)
{
    return std::string(reinterpret_cast<char*>(data.data()),
                       data.size());
}
