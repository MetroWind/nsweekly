#pragma once

#include <string>

#include "error.hpp"

class AuthInterface
{
public:
    virtual ~AuthInterface() = default;
    virtual E<void> authenticate() = 0;
};

// Authenticate against an OpenID Connect service.
class AuthOpenIDConnect : public AuthInterface
{
public:
    explicit AuthOpenIDConnect(const std::string& prefix);
    ~AuthOpenIDConnect() override = default;
    virtual E<void> authenticate() override;

private:
    std::string endpoint_auth;
    std::string endpoint_token;
    std::string endpoint_introspect;
    std::string endpoint_end_session;

};
