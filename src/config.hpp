#pragma once

#include <string>

struct Configuration
{
    std::string template_dir;
    std::string listen_address;
    int listen_port;
    std::string client_id;
    std::string client_secret;
    std::string openid_url_prefix;
    std::string url_prefix;
};
