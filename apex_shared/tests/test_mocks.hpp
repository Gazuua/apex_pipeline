#pragma once

/// Shared 어댑터 테스트용 Mock 객체 모음.

#include <apex/core/result.hpp>

#include <string>
#include <vector>

namespace apex::test
{

/// Mock PgConnection — 쿼리 실행 이력 추적 + 상태 stub.
/// 실제 libpq 연결 없이 PgTransaction의 상태 전이를 검증하는 데 사용.
class MockPgConnection
{
  public:
    bool connected = true;
    bool poisoned = false;
    std::vector<std::string> executed_queries;

    [[nodiscard]] bool is_connected() const
    {
        return connected;
    }
    [[nodiscard]] bool is_valid() const
    {
        return connected && !poisoned;
    }
    [[nodiscard]] bool is_poisoned() const
    {
        return poisoned;
    }
    void mark_poisoned() noexcept
    {
        poisoned = true;
    }
    void close()
    {
        connected = false;
    }
};

} // namespace apex::test
