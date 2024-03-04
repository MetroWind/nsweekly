#pragma once

#include <string>

#include <gmock/gmock.h>

#include "http_client.hpp"
#include "error.hpp"

class HTTPSessionMock : public HTTPSessionInterface
{
public:
    ~HTTPSessionMock() override = default;
    MOCK_METHOD(E<const HTTPResponse*>, get, (const std::string& uri),
                (override));
    MOCK_METHOD(E<const HTTPResponse*>, post,
                (const std::string& uri, const std::string& content_type,
                 const std::string& req_data), (override));
};
