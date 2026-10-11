#include "url.hpp"
#include "http_adaptation.hpp"

void copyToHttplibReq(const HTTPRequest& src, httplib::Request& dest)
{
    std::string type = "text/plain";
    if(auto it = src.header.find("Content-Type");
       it != std::end(src.header))
    {
        type = src.header.at("Content-Type");
    }
    dest.set_header("Content-Type", type);
    dest.body = src.request_data;
    for(const auto& [key, value]: src.header)
    {
        if(key != "Content-Type")
        {
            dest.set_header(key, value);
        }
    }
}


std::string httpOrigin(const std::string &text)
{
    auto url = URL::fromStr(text);
    if(!url || (url->scheme() != "http" && url->scheme() != "https") ||
       !url->user().empty() || !url->password().empty())
    {
        return "";
    }
    auto port = url->port();
    if((url->scheme() == "http" && port == "80") ||
       (url->scheme() == "https" && port == "443"))
    {
        port.clear();
    }
    return url->scheme() + "://" + url->host() +
           (port.empty() ? "" : ":" + port);
}
