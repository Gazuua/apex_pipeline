# BACKLOG-105, BACKLOG-4 — Chat TOCTOU 레이스 수정 + 크래시 핸들러 도입

- **브랜치**: `bugfix/backlog-105-4-toctou-race-and-assertion-logging`
- **PR**: #68
- **완료일**: 2026-03-21

---

## 작업 결과

### BACKLOG-105: Chat join_room TOCTOU 레이스 수정

`ChatService::on_join_room()`의 SISMEMBER→SCARD→SADD 비원자적 3단계를 Redis Lua 스크립트 단일 EVAL로 통합. Redis 싱글스레드 특성에 의해 check-then-act가 원자적으로 실행되어 동시 입장 시 max_members 초과 불가.

**변경**: `chat_service.cpp` — 기존 5단계 Redis 호출을 EVAL 1회로 교체.

### BACKLOG-4: Assertion 크래시 위치 로깅

두 구성 요소 도입:

1. **APEX_ASSERT 매크로** (`assert.hpp`): `std::source_location` 기반으로 파일/함수/라인을 spdlog critical 레벨로 기록 후 abort. Release 빌드에서도 활성.
2. **Crash signal handler** (`crash_handler.hpp/cpp`): SIGABRT/SIGSEGV/SIGFPE/SIGBUS 포착 → stderr 최소 출력(signal-safe) → spdlog best-effort flush → SIG_DFL 복원 + re-raise(core dump 허용). Windows SEH 지원.

**추가 개선** (auto-review 반영):
- Atomic 캐시 로거로 signal handler에서 spdlog registry mutex 우회
- `finalize_shutdown()`에서 `uninstall_crash_handlers()` 호출 추가
- Sanitizer 빌드(ASAN/TSAN/MSAN) 자동 감지하여 crash handler no-op

**변경 파일**: `assert.hpp`(신규), `crash_handler.hpp`(신규), `crash_handler.cpp`(신규), `server.hpp`(assert 4건 교체), `server.cpp`(install/uninstall 통합), `CMakeLists.txt`(소스 추가)
