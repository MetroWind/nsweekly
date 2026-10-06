#pragma once
#include <httplib.h>
#include "session_service.hpp"

// Registers and handles OpenID login and callback routes.
class AuthModule
{
public:
    // Borrows authentication and the shared cookie facility.
    AuthModule(AuthInterface& backend, SessionService& service)
        : auth(backend), sessions(service) {}
    // Keeps route callback targets at a stable address.
    AuthModule(const AuthModule&) = delete;
    AuthModule& operator=(const AuthModule&) = delete;
    AuthModule(AuthModule&&) = delete;
    AuthModule& operator=(AuthModule&&) = delete;
    // Registers login and callback handlers before serving requests.
    void registerRoutes(httplib::Server& server);
    // Redirects to the provider's initial login URL.
    void handleLogin(httplib::Response& res) const;
    // Exchanges the callback code and sets token cookies.
    void handleOpenIDRedirect(const httplib::Request& req,
                              httplib::Response& res) const;
private:
    AuthInterface& auth;
    SessionService& sessions;
};
