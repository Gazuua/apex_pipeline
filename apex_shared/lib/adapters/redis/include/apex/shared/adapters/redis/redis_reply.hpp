// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <hiredis/hiredis.h>

#include <cstdint>
#include <string>
#include <vector>

namespace apex::shared::adapters::redis
{

/// hiredis redisReply* wrapper.
/// Copies data from the reply pointer so it can outlive the hiredis callback scope.
struct RedisReply
{
    std::string str;               ///< STRING/STATUS response
    int64_t integer{0};            ///< INTEGER response
    int type{0};                   ///< REDIS_REPLY_* type constant
    std::vector<RedisReply> array; ///< ARRAY response (recursive)

    explicit RedisReply(redisReply* r)
    {
        if (!r)
            return;
        type = r->type;
        if (r->str)
            str.assign(r->str, r->len);
        integer = r->integer;
        // ARRAY recursive parsing
        if (r->type == REDIS_REPLY_ARRAY && r->element)
        {
            array.reserve(r->elements);
            for (size_t i = 0; i < r->elements; ++i)
            {
                array.emplace_back(r->element[i]);
            }
        }
    }

    RedisReply() = default;

    // Type check helpers
    [[nodiscard]] bool is_string() const noexcept
    {
        return type == REDIS_REPLY_STRING || type == REDIS_REPLY_STATUS;
    }
    [[nodiscard]] bool is_integer() const noexcept
    {
        return type == REDIS_REPLY_INTEGER;
    }
    [[nodiscard]] bool is_array() const noexcept
    {
        return type == REDIS_REPLY_ARRAY;
    }
    [[nodiscard]] bool is_nil() const noexcept
    {
        return type == REDIS_REPLY_NIL;
    }
    [[nodiscard]] bool is_error() const noexcept
    {
        return type == REDIS_REPLY_ERROR;
    }
};

} // namespace apex::shared::adapters::redis
