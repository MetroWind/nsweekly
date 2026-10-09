#include "open_graph.hpp"
#include "game.hpp"

nlohmann::json openGraph(const std::string &url_prefix,
    const std::string &title, const std::string &path,
    const std::string &description)
{
    auto base = url_prefix;
    while(!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }
    const auto canonical = path.substr(0, path.find_first_of("?#"));
    return {{"title", gameEscape(title)},
            {"url", gameEscape(base + canonical)},
            {"description", gameEscape(description)},
            {"image", gameEscape(base + "/statics/icon-180.png")}};
}
