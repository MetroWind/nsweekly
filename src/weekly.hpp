#pragma once

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

    static bool isValidFormatInt(int i);
};
