# BACKLOG-52 Auto-Review 보고서

## 리뷰 대상

| 항목 | 값 |
|------|-----|
| PR | #73 |
| 브랜치 | feature/backlog-52-logging-overhaul |
| 변경 | 18 files, +327/-18 |
| 리뷰어 | design, logic |

## 발견 이슈 및 조치

### CRITICAL: Redis 명령어 전체 문자열 로그 노출

- **파일**: `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp:68`
- **문제**: `spdlog::debug("[redis_multiplexer] command: {}", cmd)` — Redis 명령 전체 문자열(AUTH 비밀번호, SET 토큰 등 포함 가능)이 debug 로그에 출력
- **수정**: 명령 verb(첫 단어)만 추출하여 출력: `cmd.substr(0, cmd.find(' '))`
- **상태**: 수정 완료

### MAJOR: `core_id_for_log_()` trailing underscore 컨벤션 위반

- **파일**: `apex_core/include/apex/core/service_base.hpp`
- **문제**: private 멤버 함수에 trailing underscore 사용 — 프로젝트에서 trailing underscore는 멤버 변수 전용 컨벤션
- **수정**: `core_id_for_log_()` → `core_id_for_log()` (4개소)
- **상태**: 수정 완료

### MINOR: ErrorCode::Unknown 사용 이유 주석 삭제

- **파일**: `apex_core/src/core_engine.cpp:158-160`
- **문제**: `post_to()`에서 `ErrorCode::Unknown` 사용 이유를 설명하는 주석 3줄이 로그 추가 중 실수로 삭제됨
- **수정**: 주석 복원
- **상태**: 수정 완료

## 참고 사항 (수정 불필요)

| ID | 등급 | 내용 |
|----|------|------|
| I-1 | MINOR | `log_helpers.hpp`는 `spdlog::trace()` 직접 호출, ServiceBase 매크로는 `spdlog::log(level, ...)` 호출 — 동작 동일하나 스타일 불일치. 의도적 차이 (standalone vs 매크로) |
| I-3 | MINOR | `core_engine.hpp:spawn_tracked()` 인라인 코드가 `spdlog::` 직접 호출. `.cpp`는 `log::` 헬퍼 전환 완료. 헤더에서는 `log_helpers.hpp` include를 추가해야 하므로 현재 패턴 유지 |
| I-5 | MINOR | `message_dispatcher.cpp:33` — `spdlog::trace` 호출에 core_id 없음. MessageDispatcher가 core_id를 보유하지 않는 구조적 한계 |

## 검증 통과 항목

- 민감 데이터 노출: 전체 확인 완료 (Redis CRITICAL 1건 수정)
- 변수 참조 정확성: 전체 확인 완료
- 로그 레벨 적절성: 설계 스펙과 일치
- 핫패스 성능: 핫패스는 전부 trace/debug + should_log guard
- 포맷 문자열 정확성: 컴파일 타임 fmt::format_string 검증 + 수동 확인
- 매크로 위생: #undef 즉시 적용 확인
- 템플릿 인스턴스화: perfect forwarding 정확
