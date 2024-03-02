#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <variant>

struct RuntimeError
{
    std::string msg;
};

struct HTTPError
{
    int code;
    std::string msg;
};

using Error = std::variant<RuntimeError, HTTPError>;

template<class T>
using E = std::expected<T, Error>;

inline Error runtimeError(std::string_view msg)
{
    return RuntimeError{std::string(msg)};
}

inline Error httpError(int code, std::string_view msg)
{
    return HTTPError{code, std::string(msg)};
}
