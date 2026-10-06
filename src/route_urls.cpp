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

