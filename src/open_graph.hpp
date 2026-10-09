#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Builds escaped Open Graph fields using the configured public URL prefix.
nlohmann::json openGraph(const std::string &url_prefix,
    const std::string &title, const std::string &path,
    const std::string &description);
