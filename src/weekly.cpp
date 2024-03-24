#include <expected>

#include <cmark.h>
#include <spdlog/spdlog.h>

#include "error.hpp"
#include "weekly.hpp"

E<std::string> renderMarkdown(const std::string& src)
{
    char* html = cmark_markdown_to_html(src.data(), src.size(),
                                        CMARK_OPT_DEFAULT);
    if(html == nullptr)
    {
        return std::unexpected(runtimeError("Failed to render Markdown."));
    }
    std::string result = html;
    free(html);
    return result;
}

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
