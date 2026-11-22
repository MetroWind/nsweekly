#pragma once

#include <httplib.h>

#include "music_data.hpp"
#include "user_data.hpp"

// Implements the ListenBrainz protocol over Music storage.
class ListenBrainzApi
{
public:
    // Borrows Music storage and the shared account directory.
    ListenBrainzApi(MusicDataInterface& data, UserDataInterface& user_data)
        : music(data), users(user_data) {}
    // Registers the supported v1 API routes.
    void registerRoutes(httplib::Server& server);

private:
    struct ReadAccount
    {
        int64_t owner_id;
        bool include_additional_info;
    };

    E<ReadAccount> readAccount(const httplib::Request& req,
                               const std::string& name);
    E<std::optional<MusicIdentity>> authenticate(const httplib::Request& req);
    void validate(const httplib::Request& req, httplib::Response& res);
    void submit(const httplib::Request& req, httplib::Response& res,
                const httplib::ContentReader& reader);
    void history(const httplib::Request& req, httplib::Response& res);
    void playingNow(const httplib::Request& req, httplib::Response& res);

    MusicDataInterface& music;
    UserDataInterface& users;
};
