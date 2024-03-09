#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <span>
#include <unordered_map>

#include <curl/curl.h>

#include "error.hpp"

struct HTTPRequest
{
    std::string url;
    std::string request_data;
    std::unordered_map<std::string, std::string> header;

    HTTPRequest() = default;
    explicit HTTPRequest(std::string_view uri) : url(uri) {}
    HTTPRequest& setPayload(std::string_view data)
    {
        request_data = data;
        return *this;
    }
    HTTPRequest& addHeader(std::string_view key, std::string_view value)
    {
        header.emplace(key, value);
        return *this;
    }
    bool operator==(const HTTPRequest&) const = default;
};

struct HTTPResponse
{
    int status;
    std::vector<std::byte> payload;
    std::unordered_map<std::string, std::string> header;

    HTTPResponse() = default;
    HTTPResponse(int status_code, std::string_view payload_str);
    void clear();
    std::string_view payloadAsStr() const;
};

class HTTPSessionInterface
{
public:
    virtual ~HTTPSessionInterface() = default;
    virtual E<const HTTPResponse*> get(const std::string& uri) = 0;
    virtual E<const HTTPResponse*> post(
        const std::string& uri, const std::string& content_type,
        const std::string& req_data)
    {
        return this->post(HTTPRequest(uri).setPayload(req_data)
                          .addHeader("Content-Type", content_type));
    }
    virtual E<const HTTPResponse*> post(HTTPRequest req) = 0;
};

// Threads should not share session
class HTTPSession : public virtual HTTPSessionInterface
{
public:
    HTTPSession();
    ~HTTPSession() override;
    HTTPSession(const HTTPSession&);
    HTTPSession& operator=(const HTTPSession&) = delete;

    // The returned pointer is garenteed to be non-null.
    E<const HTTPResponse*> get(const std::string& uri) override;
    using HTTPSessionInterface::post;
    E<const HTTPResponse*> post(HTTPRequest req) override;

private:
    CURL* handle = nullptr;
    HTTPResponse res;

    void prepareForNewRequest();

    static size_t writeResponse(char *ptr, size_t size, size_t nmemb,
                                void *res_buffer);
    static size_t writeHeaders(char *buffer, size_t size, size_t nitems,
                               void *userdata);
};
