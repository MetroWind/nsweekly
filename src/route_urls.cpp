#include "route_urls.hpp"

std::string weeklyURL(const std::string& arg)
{
    return "/weekly/" + arg;
}

std::string editURL(const std::string& arg)
{
    return "/edit/" + arg;
}

std::string urlFor(const std::string& name, const std::string& arg)
{
    if(name == "games") { return gamesURL(arg); }
    if(name == "index")
    {
        return "/";
    }
    if(name == "openid-redirect")
    {
        return "/openid-redirect";
    }
    if(name == "weekly")
    {
        return weeklyURL(arg);
    }
    if(name == "statics")
    {
        return "/statics/" + arg;
    }
    if(name == "login")
    {
        return "/login";
    }
    if(name == "edit")
    {
        // Arg is expected to be in username/YYYY-MM-DD format.
        return editURL(arg);
    }
    return "";
}


std::string gamesURL(const std::string& username)
{
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string result = "/games/";
    for(unsigned char c: username)
    {
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~')
        { result += static_cast<char>(c); }
        else
        {
            result += '%';
            result += HEX[c >> 4];
            result += HEX[c & 15];
        }
    }
    return result;
}
