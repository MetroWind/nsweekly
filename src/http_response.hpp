#pragma once
#include <httplib.h>
#include "error.hpp"
#include "utils.hpp"

#define _ASSIGN_OR_RESPOND_ERROR(tmp, var, val, res)                    \
    auto tmp = val;                                                     \
    if(!tmp.has_value())                                                \
    {                                                                   \
        if(std::holds_alternative<HTTPError>(tmp.error()))              \
        {                                                               \
            const HTTPError& e = std::get<HTTPError>(tmp.error());      \
            res.status = e.code;                                        \
            res.set_content(e.msg, "text/plain");                       \
            return;                                                     \
        }                                                               \
        else                                                            \
        {                                                               \
            res.status = 500;                                           \
            res.set_content(std::visit([](const auto& e) { return e.msg; }, \
                                       tmp.error()),                    \
                            "text/plain");                              \
            return;                                                     \
        }                                                               \
    }                                                                   \
    var = std::move(tmp).value()

// Assigns an expected value or sends its existing HTTP error response.
// Val should be an rvalue.
#define ASSIGN_OR_RESPOND_ERROR(var, val, res)                          \
    _ASSIGN_OR_RESPOND_ERROR(_CONCAT_NAMES(assign_or_return_tmp, __COUNTER__), \
                            var, val, res)

