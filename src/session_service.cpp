#include <unordered_map>
#include <spdlog/spdlog.h>

#include "session_service.hpp"
#include "url.hpp"

namespace
{
std::unordered_map<std::string, std::string> parseCookies(
    std::string_view value)
{
    std::unordered_map<std::string, std::string> cookies;
    size_t begin = 0;
    while(true)
    {
        if(begin >= value.size())
        {
            break;
        }

        size_t semicolon = value.find(';', begin);
        if(semicolon == std::string::npos)
        {
            semicolon = value.size();
        }

        std::string_view section = value.substr(begin, semicolon - begin);

        begin = semicolon + 1;
        // Skip spaces
        while(begin < value.size() && value[begin] == ' ')
        {
            begin++;
        }

        size_t equal = section.find('=');
        if(equal == std::string::npos) continue;
        cookies.emplace(section.substr(0, equal),
                        section.substr(equal+1, semicolon - equal - 1));
        if(semicolon >= value.size())
        {
            continue;
        }
    }
    return cookies;
}

} // namespace

void SessionService::setTokenCookies(
    const Tokens& tokens, mw::HTTPServer::Response& res) const
{
    int64_t expire_sec = 300;
    if(tokens.expiration.has_value())
    {
        auto expire = std::chrono::duration_cast<std::chrono::seconds>(
            *tokens.expiration - Clock::now());
        expire_sec = expire.count();
    }
    res.set_header("Set-Cookie", std::format(
                       "access-token={}; Max-Age={}",
                       urlEncode(tokens.access_token), expire_sec));
    // Add refresh token to cookie, with one month expiration.
    if(tokens.refresh_token.has_value())
    {
        expire_sec = 1800;
        if(tokens.refresh_expiration.has_value())
        {
            auto expire = std::chrono::duration_cast<std::chrono::seconds>(
                *tokens.refresh_expiration - Clock::now());
            expire_sec = expire.count();
        }

        res.set_header("Set-Cookie", std::format(
                           "refresh-token={}; Max-Age={}",
                           urlEncode(*tokens.refresh_token), expire_sec));
    }
}

E<SessionValidation> SessionService::validateSession(
    const mw::HTTPServer::Request& req) const
{
    if(!req.has_header("Cookie"))
    {
        spdlog::debug("Request has no cookie.");
        return SessionValidation::invalid();
    }

    auto cookies = parseCookies(req.get_header_value("Cookie"));
    if(auto it = cookies.find("access-token");
       it != std::end(cookies))
    {
        spdlog::debug("Cookie has access token.");
        Tokens tokens;
        tokens.access_token = it->second;
        E<UserInfo> user = auth.getUser(tokens);
        if(user.has_value())
        {
            return SessionValidation::valid(*std::move(user));
        }
    }
    // No access token or access token expired
    if(auto it = cookies.find("refresh-token");
       it != std::end(cookies))
    {
        spdlog::debug("Cookie has refresh token.");
        // Try to refresh the tokens.
        ASSIGN_OR_RETURN(Tokens tokens, auth.refreshTokens(it->second));
        ASSIGN_OR_RETURN(UserInfo user, auth.getUser(tokens));
        return SessionValidation::refreshed(std::move(user), std::move(tokens));
    }
    return SessionValidation::invalid();
}

