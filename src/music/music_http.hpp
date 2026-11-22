#pragma once

#include <httplib.h>

#include "error.hpp"

// Base path of the supported ListenBrainz protocol version.
inline constexpr char LISTENBRAINZ_API_PATH[] = "/music/api/listenbrainz/1";

// Generates a one-time form nonce from 32 random bytes encoded as hex.
E<std::string> musicNonce();

// Generates a device token from 16 random bytes encoded as unpadded Base64.
E<std::string> musicDeviceToken();

// Recognizes unpadded Base64 and legacy padded Base64 or hex device tokens.
bool validMusicDeviceToken(std::string_view token);

// Hashes the wire token for storage and lookup.
E<std::string> musicDigest(const std::string& token);

// Removes surrounding ASCII whitespace from a form or metadata field.
std::string musicTrim(std::string_view text);

// Parses a complete decimal integer.
std::optional<int64_t> musicInteger(std::string_view text);

// Returns the current Unix timestamp in whole seconds.
int64_t musicEpochSeconds();

// Matches a media type case-insensitively, allowing parameters.
bool musicMediaType(const httplib::Request& req, std::string_view expected);

// Reads a route-local bounded body, including requests without Content-Length.
E<std::string> musicBody(const httplib::Request& req, httplib::Response& res,
                        const httplib::ContentReader& reader, size_t limit);
