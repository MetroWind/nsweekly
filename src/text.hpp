#pragma once
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

// Accepts well-formed Unicode text without embedded NUL bytes.
inline bool validUtf8Text(std::string_view text)
{
    for(size_t i = 0; i < text.size();)
    {
        auto c = static_cast<unsigned char>(text[i++]);
        if(c == 0)
        {
            return false;
        }
        if(c < 128)
        {
            continue;
        }
        unsigned count;
        uint32_t value;
        uint32_t minimum;
        if(c >= 0xc2 && c <= 0xdf)
        {
            count = 1;
            value = c & 31;
            minimum = 0x80;
        }
        else if(c >= 0xe0 && c <= 0xef)
        {
            count = 2;
            value = c & 15;
            minimum = 0x800;
        }
        else if(c >= 0xf0 && c <= 0xf4)
        {
            count = 3;
            value = c & 7;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }
        if(i + count > text.size())
        {
            return false;
        }
        while(count--)
        {
            auto next = static_cast<unsigned char>(text[i++]);
            if((next & 0xc0) != 0x80)
            {
                return false;
            }
            value = (value << 6) | (next & 63);
        }
        if(value < minimum || value > 0x10ffff ||
           (value >= 0xd800 && value <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}

// Parses a four-digit ISO calendar date with no time or offset.
inline std::optional<std::chrono::year_month_day>
parseCalendarDate(std::string_view text)
{
    if(text.size() != 10 || text[4] != '-' || text[7] != '-')
    {
        return {};
    }
    int parts[3]{};
    const size_t offsets[]{0, 5, 8};
    const size_t widths[]{4, 2, 2};
    for(size_t i = 0; i < 3; ++i)
    {
        auto part = text.substr(offsets[i], widths[i]);
        if(!std::ranges::all_of(part, [](char c) { return c >= '0' && c <= '9'; }))
        {
            return {};
        }
        std::from_chars(part.data(), part.data() + part.size(), parts[i]);
    }
    std::chrono::year_month_day date{std::chrono::year(parts[0]),
                                     std::chrono::month(parts[1]),
                                     std::chrono::day(parts[2])};
    if(parts[0] < 1 || !date.ok())
    {
        return {};
    }
    return date;
}
