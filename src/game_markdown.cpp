#include <macrodown.h>
#include <exception>
#include "game_markdown.hpp"

E<std::string> renderGameMarkdown(const std::string &source)
{
    try
    {
        macrodown::MacroDown processor;
        auto root = processor.parse(source);
        if(!root)
        {
            return std::unexpected(runtimeError("MacroDown parse failed"));
        }
        return processor.render(*root);
    }
    catch(const std::exception &)
    {
        return std::unexpected(runtimeError("MacroDown rendering failed"));
    }
}
