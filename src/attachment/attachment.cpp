#include "attachment/attachment.hpp"
#include <algorithm>
#include <mw/utils.hpp>

bool attachmentHash(std::string_view hash)
{
    return hash.size() == 32 &&
           std::ranges::all_of(hash,
                               [](char c)
                               {
                                   return (c >= '0' && c <= '9') ||
                                          (c >= 'a' && c <= 'f');
                               });
}

E<std::string> attachmentName(std::string_view name)
{
    const auto supplied = name;
    auto separator = name.find_last_of("/\\");
    if(separator != name.npos)
    {
        name.remove_prefix(separator + 1);
    }
    bool valid =
        !name.empty() && name.size() <= 1024 && name != "." && name != "..";
    for(size_t i = 0; valid && i < supplied.size();)
    {
        auto c = static_cast<unsigned char>(supplied[i++]);
        if(c < 32 || c == 127)
        {
            valid = false;
            break;
        }
        if(c < 128)
        {
            continue;
        }
        int count = c >= 0xc2 && c <= 0xdf   ? 1
                    : c >= 0xe0 && c <= 0xef ? 2
                    : c >= 0xf0 && c <= 0xf4 ? 3
                                             : -1;
        uint32_t code = count == 1 ? c & 31 : count == 2 ? c & 15 : c & 7;
        if(count < 0 || i + count > supplied.size())
        {
            valid = false;
            break;
        }
        for(int j = 0; j < count; ++j)
        {
            auto next = static_cast<unsigned char>(supplied[i++]);
            if((next & 0xc0) != 0x80)
            {
                valid = false;
            }
            code = (code << 6) | (next & 63);
        }
        if((count == 1 && code < 128) || (count == 2 && code < 2048) ||
           (count == 3 && code < 65536) || code > 0x10ffff ||
           (code >= 0xd800 && code <= 0xdfff) || (code >= 0x80 && code <= 0x9f))
        {
            valid = false;
        }
    }
    if(!valid)
    {
        return std::unexpected(
            httpError(422, "Invalid filename (UTF-8, "
                           "1–1024 bytes, no controls, '.' or '..' required)"));
    }
    return std::string(name);
}

std::string attachmentSegment(std::string_view name)
{
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    for(unsigned char c : name)
    {
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~')
        {
            result += c;
        }
        else
        {
            result += '%';
            result += HEX[c >> 4];
            result += HEX[c & 15];
        }
    }
    return result;
}

E<std::string> attachmentURL(std::string_view hash, std::string_view name)
{
    if(!attachmentHash(hash))
    {
        return std::unexpected(httpError(400, "Invalid hash"));
    }
    ASSIGN_OR_RETURN(auto basename, attachmentName(name));
    return "/attachment/" + std::string(hash) + "/" +
           attachmentSegment(basename);
}

E<std::filesystem::path> attachmentPath(const std::filesystem::path& root,
                                        std::string_view hash)
{
    if(!attachmentHash(hash))
    {
        return std::unexpected(httpError(400, "Invalid hash"));
    }
    return root / hash.substr(0, 2) / hash;
}
