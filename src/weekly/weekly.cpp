#include <expected>

#include "markdown.hpp"
#include "weekly/weekly.hpp"

bool WeeklyPost::isValidFormatInt(int i)
{
    switch(i)
    {
    case MARKDOWN:
        return true;
    }
    return false;
}

E<std::string> WeeklyPost::render() const
{
    switch(format)
    {
    case MARKDOWN:
        return renderMarkdown(raw_content);
    default:
        return std::unexpected(runtimeError(
            "Somebody forgot to add a switch case for a weekly format!"));
    }
}
