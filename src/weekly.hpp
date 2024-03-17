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
};
