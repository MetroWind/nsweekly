#pragma once

#include "error.hpp"
#include "utils.hpp"

class WeeklyPost
{
public:
    enum Format
    {
        MARKDOWN,
    };

    Format format;
    std::string raw_content;
    Time week_begin;
    Time update_time;
    // The IETF BCP 47 language tag (RFC 5646) of the post.
    std::string language;
    std::string author;

    static bool isValidFormatInt(int i);
    // Render the post to HTML.
    E<std::string> render() const;
};
