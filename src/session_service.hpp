#pragma once
#include <mw/http_server.hpp>
#include "auth.hpp"

// Carries session validity, user identity, and any replacement tokens.
struct SessionValidation
{
    // Distinguishes unchanged, refreshed, and absent authentication.
    enum Status { VALID, REFRESHED, INVALID };
    // The outcome of validating this request's cookies.
    Status status;
    // The authenticated identity for valid and refreshed sessions.
    UserInfo user;
    // Replacement cookies available after a successful refresh.
    Tokens new_tokens;

    // Creates a valid session without replacement tokens.
    static SessionValidation valid(UserInfo&& user_info)
    {
        return {VALID, std::move(user_info), {}};
    }

    // Creates a refreshed session carrying replacement tokens.
    static SessionValidation refreshed(UserInfo&& user_info, Tokens&& tokens)
    {
        return {REFRESHED, std::move(user_info), std::move(tokens)};
    }

    // Creates a session without authenticated identity.
    static SessionValidation invalid()
    {
        return {INVALID, {}, {}};
    }
};

// Shares cookie validation and token formatting between HTTP modules.
class SessionService
{
public:
    // Borrows the application's authentication backend.
    explicit SessionService(AuthInterface& backend) : auth(backend) {}
    // Validates access tokens and attempts the existing refresh fallback.
    E<SessionValidation> validateSession(
        const mw::HTTPServer::Request& req) const;
    // Applies the existing token cookie headers to a response.
    void setTokenCookies(
        const Tokens& tokens, mw::HTTPServer::Response& res) const;
private:
    AuthInterface& auth;
};
