# v0.5.10.7 — ASAN UAF: socket executor ≠ core executor timer service use-after-free

**PR**: #115
**버전**: v0.5.10.7
**완료**: 2026-03-23

## 변경 요약

Session의 timer 및 write_pump가 소켓의 executor를 사용할 때, 소켓 executor와 core executor가 다른 io_context에 속하는 경우 timer service가 이미 파괴된 io_context에 접근하는 UAF를 수정했다.

## 문제

Session의 timer/write_pump 코루틴이 `socket().get_executor()`로 얻은 executor를 사용했는데, 이 executor가 core에 바인딩된 io_context의 executor와 다를 수 있었다. 소켓 executor의 io_context가 먼저 파괴되면 timer service에 대한 use-after-free가 발생했다.

## 해결

Session에 `core_executor_` 멤버를 추가하여 core의 io_context executor를 명시적으로 보관하고, timer/write_pump에서 소켓 executor 대신 core_executor_를 사용하도록 변경했다.

## 변경 파일

| 파일 | 변경 |
|------|------|
| `apex_core/include/apex/net/session.hpp` | core_executor_ 멤버 추가 |
| `apex_core/src/net/session.cpp` | timer/write_pump에서 core_executor_ 사용 |
| `apex_core/include/apex/net/connection_handler.hpp` | core executor 전달 인터페이스 조정 |

## 검증

CI 전체 통과: ASAN, TSAN, GCC, UBSAN, E2E
