#include <algorithm>
#include <charconv>
#include <format>
#include <mw/utils.hpp>
#include "game.hpp"

namespace
{
bool asciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string fieldValue(const GameFields &fields, const std::string &key)
{
    auto it = fields.scalars.find(key);
    return it == fields.scalars.end() ? std::string{} : it->second;
}

std::string stripped(const std::string &value)
{
    return std::string(mw::strip(value));
}

template <typename T, size_t N>
std::optional<T> parseChoice(const std::array<GameChoice<T>, N> &choices,
                             std::string_view code)
{
    for(const auto &choice : choices)
    {
        if(choice.code == code)
        {
            return choice.value;
        }
    }
    return {};
}

template <typename T, size_t N>
std::string choiceCode(const std::array<GameChoice<T>, N> &choices, T value)
{
    for(const auto &choice : choices)
    {
        if(choice.value == value)
        {
            return std::string(choice.code);
        }
    }
    return "invalid";
}

std::optional<std::chrono::year_month_day> parseDate(std::string_view text)
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
        if(!std::ranges::all_of(part, asciiDigit))
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
} // namespace

bool validGameText(std::string_view text)
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

std::string gameEscape(std::string_view text)
{
    return mw::escapeHTML(text);
}

std::string gameDate(std::chrono::year_month_day date)
{
    return std::format("{:04}-{:02}-{:02}", int(date.year()),
                       unsigned(date.month()), unsigned(date.day()));
}

E<GameInput> validateGameInput(const GameFields &fields)
{
    GameInput input;
    GameValidationError error;
    for(const auto &[key, value] : fields.scalars)
    {
        if(!validGameText(value))
        {
            error.fields[key] = "Invalid UTF-8 or NUL";
        }
    }
    input.name = stripped(fieldValue(fields, "name"));
    if(input.name.empty())
    {
        error.fields["name"] = "Name is required";
    }
    auto status =
        parseChoice(STATUS_CHOICES, stripped(fieldValue(fields, "status")));
    if(!status)
    {
        error.fields["status"] = "Choose a status";
    }
    else
    {
        input.status = *status;
    }
    auto completion = stripped(fieldValue(fields, "completion"));
    if(!completion.empty())
    {
        input.completion = parseChoice(COMPLETION_CHOICES, completion);
        if(!input.completion)
        {
            error.fields["completion"] = "Unknown completion";
        }
    }
    for(const auto &code : fields.platforms)
    {
        if(!parseChoice(PLATFORM_CHOICES, code))
        {
            error.fields["platforms"] = "Unknown platform";
        }
    }
    for(const auto &choice : PLATFORM_CHOICES)
    {
        if(std::ranges::find(fields.platforms, choice.code) !=
           fields.platforms.end())
        {
            input.platforms.push_back(choice.value);
        }
    }
    auto hours = stripped(fieldValue(fields, "hours"));
    if(!hours.empty())
    {
        int32_t value = 0;
        auto [end, ec] =
            std::from_chars(hours.data(), hours.data() + hours.size(), value);
        if(ec != std::errc{} || end != hours.data() + hours.size() ||
           !std::ranges::all_of(hours, asciiDigit))
        {
            error.fields["hours"] = "Use an integer from 0 to 2147483647";
        }
        else
        {
            input.hours = value;
        }
    }
    for(const auto &key : {"start_date", "end_date"})
    {
        auto text = stripped(fieldValue(fields, key));
        if(text.empty())
        {
            continue;
        }
        auto date = parseDate(text);
        if(!date)
        {
            error.fields[key] = "Use a valid YYYY-MM-DD date";
        }
        else if(std::string_view(key) == "start_date")
        {
            input.start_date = date;
        }
        else
        {
            input.end_date = date;
        }
    }
    if(input.start_date && input.end_date &&
       *input.end_date < *input.start_date)
    {
        error.fields["end_date"] = "End date precedes start date";
    }
    auto notes = stripped(fieldValue(fields, "notes"));
    if(!notes.empty())
    {
        input.notes = std::move(notes);
    }
    if(!error.fields.empty())
    {
        return std::unexpected(Error(error));
    }
    return input;
}

GameFields gameFields(const GameInput &input)
{
    GameFields fields;
    fields.scalars = {
        {"name", input.name},
        {"status", choiceCode(STATUS_CHOICES, input.status)},
        {"completion", input.completion
                           ? choiceCode(COMPLETION_CHOICES, *input.completion)
                           : ""},
        {"hours", input.hours ? std::to_string(*input.hours) : ""},
        {"start_date", input.start_date ? gameDate(*input.start_date) : ""},
        {"end_date", input.end_date ? gameDate(*input.end_date) : ""},
        {"notes", input.notes.value_or("")}};
    for(auto value : input.platforms)
    {
        fields.platforms.push_back(choiceCode(PLATFORM_CHOICES, value));
    }
    return fields;
}

E<GameInput> validateGameInput(const GameInput &input)
{
    return validateGameInput(gameFields(input));
}
