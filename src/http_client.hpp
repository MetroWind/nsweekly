#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <span>
#include <unordered_map>

#include <curl/curl.h>

#include "error.hpp"

struct HTTPResponse
{
    int status;
    std::vector<std::byte> payload;
    std::unordered_map<std::string, std::string> header;

    void clear();
};

// Threads should not share session
class HTTPSession
{
public:
    HTTPSession();
    ~HTTPSession();
    HTTPSession(const HTTPSession&);
    HTTPSession& operator=(const HTTPSession&) = delete;

    // The returned pointer is garenteed to be non-null.
    E<const HTTPResponse*> get(const std::string& uri);
    E<const HTTPResponse*> post(const std::string& uri,
                                const std::string& content_type,
                                const std::string& req_data);

private:
    CURL* handle = nullptr;
    HTTPResponse res;

    void prepareForNewRequest();

    static size_t writeResponse(char *ptr, size_t size, size_t nmemb,
                                void *res_buffer);
    static size_t writeHeaders(char *buffer, size_t size, size_t nitems,
                               void *userdata);
};
