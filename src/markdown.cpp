#include <macrodown.h>
#include <exception>
#include "markdown.hpp"

E<std::string> renderMarkdown(const std::string &source)
{
    try
    {
        macrodown::MacroDown processor;
        auto root = processor.parse(source);
        if(!root)
        {
            return std::unexpected(runtimeError("Markdown parse failed"));
        }
        return processor.render(*root);
    }
    catch(const std::exception &)
    {
        return std::unexpected(runtimeError("Markdown rendering failed"));
    }
}
