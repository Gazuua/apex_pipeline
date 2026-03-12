#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace apex::shared::adapters::redis {

/// Redis 어댑터 설정. TOML [adapters.redis] 섹션에서 파싱.
struct RedisConfig {
    std::string host = "localhost";
    uint16_t port = 6379;
    std::string password;                                   ///< AUTH 비밀번호 (빈 문자열 = 미사용)
    uint32_t db = 0;                                        ///< SELECT DB 번호

    size_t pool_size_per_core = 3;                          ///< 코어당 풀 크기
    size_t pool_max_size_per_core = 8;                      ///< 코어당 최대 확장 한도
    std::chrono::seconds max_idle_time{60};                 ///< 유휴 커넥션 폐기 시간
    std::chrono::seconds health_check_interval{30};         ///< 헬스 체크 주기

    std::chrono::milliseconds connect_timeout{3000};        ///< 커넥션 타임아웃
    std::chrono::milliseconds command_timeout{1000};        ///< 커맨드 타임아웃
};

} // namespace apex::shared::adapters::redis
