// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace apex::shared::adapters::redis
{

/// Redis 어댑터 설정. TOML [adapters.redis] 섹션에서 파싱.
struct RedisConfig
{
    std::string host = "localhost";
    uint16_t port = 6379;
    std::string password; ///< AUTH 비밀번호 (빈 문자열 = 미사용)
    uint32_t db = 0;      ///< SELECT DB 번호

    std::chrono::milliseconds connect_timeout{3000};        ///< 커넥션 타임아웃
    std::chrono::milliseconds command_timeout{1000};        ///< 커맨드 타임아웃
    std::chrono::milliseconds reconnect_max_backoff{30000}; ///< 재연결 최대 백오프
    uint32_t max_pending_commands = 4096;                   ///< pending command slab 최대 수
};

} // namespace apex::shared::adapters::redis
