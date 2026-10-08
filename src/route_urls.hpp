#pragma once
#include <string>

// Builds a weekly list or dated weekly path from its existing argument.
std::string weeklyURL(const std::string& arg);
// Builds an edit path from username/date.
std::string editURL(const std::string& arg);
// Preserves the named URL mapping used by templates.
std::string urlFor(const std::string& name, const std::string& arg);

// Encodes a username as one path component in the games namespace.
std::string gamesURL(const std::string& username);
