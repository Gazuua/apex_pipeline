#pragma once

#include <hiredis/hiredis.h>

#include <cstdint>
#include <string>

namespace apex::shared::adapters::redis {

/// hiredis redisReply* wrapper.
/// Copies data from the reply pointer so it can outlive the hiredis callback scope.
struct RedisReply {
    std::string str;      ///< STRING/STATUS response
    int64_t integer{0};   ///< INTEGER response
    int type{0};          ///< REDIS_REPLY_* type constant

    explicit RedisReply(redisReply* r) {
        if (!r) return;
        type = r->type;
        if (r->str) str.assign(r->str, r->len);
        integer = r->integer;
    }

    RedisReply() = default;
};

} // namespace apex::shared::adapters::redis
