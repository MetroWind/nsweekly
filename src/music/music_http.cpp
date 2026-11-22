#include <algorithm>
#include <charconv>
#include <chrono>
#include <mw/crypto.hpp>

#include "music_http.hpp"
#include "utils.hpp"

E<std::string> musicNonce()
{
    ASSIGN_OR_RETURN(auto bytes, mw::Crypto{}.randomBytes(32));
    constexpr std::string_view HEX = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for(auto byte : bytes)
    {
        auto value = std::to_integer<unsigned>(byte);
        token += HEX[value >> 4];
        token += HEX[value & 15];
    }
    return token;
}

namespace
{
constexpr size_t DEVICE_TOKEN_BYTES = 16;
constexpr size_t DEVICE_TOKEN_LENGTH = (DEVICE_TOKEN_BYTES * 8 + 5) / 6;
constexpr std::string_view BASE64_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool usesAlphabet(std::string_view text, std::string_view alphabet)
{
    return text.find_first_not_of(alphabet) == std::string_view::npos;
}
}

E<std::string> musicDeviceToken()
{
    ASSIGN_OR_RETURN(auto bytes, mw::Crypto{}.randomBytes(DEVICE_TOKEN_BYTES));
    auto data = std::span(reinterpret_cast<unsigned char*>(bytes.data()),
                          bytes.size());
    return mw::base64Encode(data);
}

bool validMusicDeviceToken(std::string_view token)
{
    if(token.size() == 64)
    {
        return usesAlphabet(token, "0123456789abcdef");
    }
    if(token.size() == DEVICE_TOKEN_LENGTH + 2 && token.ends_with("=="))
    {
        token.remove_suffix(2);
    }
    return token.size() == DEVICE_TOKEN_LENGTH &&
           usesAlphabet(token, BASE64_ALPHABET);
}

E<std::string> musicDigest(const std::string& token)
{
    return mw::SHA256Hasher{}.hashToHexStr(token);
}

std::string musicTrim(std::string_view text)
{
    constexpr std::string_view WHITESPACE = " \t\r\n\f\v";
    auto first = text.find_first_not_of(WHITESPACE);
    if(first == std::string_view::npos)
    {
        return {};
    }
    return std::string(text.substr(
        first, text.find_last_not_of(WHITESPACE) - first + 1));
}

std::optional<int64_t> musicInteger(std::string_view text)
{
    if(text.empty()) { return std::nullopt; }
    int64_t number;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(),
                                        number);
    if(error != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return number;
}

int64_t musicEpochSeconds()
{
    return std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now()).time_since_epoch().count();
}

bool musicMediaType(const httplib::Request& req, std::string_view expected)
{
    if(req.get_header_value_count("Content-Type") != 1)
    {
        return false;
    }
    auto value = req.get_header_value("Content-Type");
    auto type = musicTrim(std::string_view(value).substr(0, value.find(';')));
    return std::ranges::equal(type, expected, [](char c, char lower)
    {
        return (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) == lower;
    });
}

E<std::string> musicBody(const httplib::Request& req, httplib::Response& res,
                        const httplib::ContentReader& reader, size_t limit)
{
    if(req.has_header("Content-Length") &&
       req.get_header_value_u64("Content-Length") > limit)
    {
        return std::unexpected(httpError(413, "Request body is too large"));
    }
    std::string body;
    bool too_large = false;
    if(!reader([&](const char* data, size_t size)
    {
        if(size > limit - body.size())
        {
            too_large = true;
            return false;
        }
        body.append(data, size);
        return true;
    }))
    {
        if(too_large || res.status == 413)
        {
            return std::unexpected(httpError(413, "Request body is too large"));
        }
        return std::unexpected(httpError(400, "Could not read request body"));
    }
    return body;
}
