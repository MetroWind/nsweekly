#pragma once

#include <chrono>
#include <string>
#include <memory>
#include <optional>

#include "error.hpp"
#include "http_client.hpp"
#include "config.hpp"
#include "utils.hpp"

struct Tokens
{
    std::string access_token;
    std::optional<std::string> refresh_token;
    std::optional<Time> expiration;
};

struct UserInfo
{
    std::string id;
    std::string name;
};

// Authenticate against an OpenID Connect service.
class AuthOpenIDConnect
{
public:
    // Do not use. Use create() instead.
    AuthOpenIDConnect(const Configuration& conf, std::string_view redirect_url,
                      std::unique_ptr<HTTPSessionInterface> http)
            : redirection_url(redirect_url), http_client(std::move(http)),
              config(conf) {}

    // This will get metadata from $prefix/.well-known. The returned
    // pointer will never be null.
    static E<std::unique_ptr<AuthOpenIDConnect>> create(
        const Configuration& config, std::string_view redirect_url,
        std::unique_ptr<HTTPSessionInterface> http);
    ~AuthOpenIDConnect() = default;

    std::string initialURL() const;
    // Do a GET on the initial URL. Normally this is not needed, as
    // the client should just redirect the to the initial URL on the
    // browser.
    E<HTTPResponse> initiate() const;
    E<Tokens> authenticate(std::string_view code) const;
    // Only the access token is used in this request.
    E<UserInfo> getUser(const Tokens& tokens) const;

    E<Tokens> refreshTokens(std::string_view refresh_token) const;

private:
    std::string endpoint_auth;
    std::string endpoint_token;
    std::string endpoint_introspect;
    std::string endpoint_end_session;
    std::string endpoint_user_info;

    const std::string redirection_url;
    std::unique_ptr<HTTPSessionInterface> http_client;
    const Configuration& config;
};
