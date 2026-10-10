#pragma once
#include "error.hpp"
#include <string>

// Renders one independent MacroDown document, preserving macro HTML.
E<std::string> renderGameMarkdown(const std::string &source);
