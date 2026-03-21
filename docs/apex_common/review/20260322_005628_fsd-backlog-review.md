# Auto-Review: FSD Backlog Sweep (BACKLOG-63,124,125)

- **브랜치**: feature/fsd-backlog-20260322_003257
- **리뷰 일시**: 2026-03-22 00:56:28
- **리뷰 범위**: 8파일, +135/-25

## 리뷰 대상

| 파일 | 변경 요약 |
|------|----------|
| `apex_infra/postgres/init.sql` | 고아 스키마 3개 제거, 주석 갱신 |
| `apex_services/gateway/include/apex/gateway/jwt_blacklist.hpp` | detail namespace에 is_valid_jti 선언 추가 |
| `apex_services/gateway/src/jwt_blacklist.cpp` | anonymous → detail namespace 이동 |
| `apex_services/gateway/tests/test_jwt_blacklist.cpp` | 신규 — 16개 테스트 케이스 |
| `apex_services/gateway/CMakeLists.txt` | 테스트 타겟 추가 |
| `CLAUDE.md` | 문서/프로세스 규칙 중복 제거 |
| `docs/CLAUDE.md` | 단일 권위 출처 명확화, progress 규칙 추가 |
| `docs/BACKLOG.md` | FSD 분석 태그 5건 추가 |

## 리뷰 항목

### 1. [test] is_valid_jti 테스트 커버리지 — 보완 완료

**심각도**: Minor
**상태**: 수정 완료

초기 14개 테스트에서 하이픈 전용 JTI(`"-"`, `"---"`)와 순수 숫자 입력 테스트가 누락.
보안 검증 함수이므로 허용 문자 조합의 엣지 케이스를 보강하여 16개로 확대.

### 2. [design] detail namespace 이동 — 적절

**심각도**: Info
**상태**: 이슈 없음

`is_valid_jti`를 anonymous namespace에서 `detail` namespace로 이동하여 테스트 접근 허용.
C++ 테스트 노출의 표준 패턴. `[[nodiscard]]` 속성 추가. 헤더의 doxygen 주석이
정확하게 "hex digits + hyphens"로 기술 (기존 cpp 주석의 "alphanumeric" 오류도 해소).

### 3. [infra] init.sql 고아 스키마 제거 — 안전

**심각도**: Info
**상태**: 이슈 없음

`auth_schema`, `chat_schema`, `match_schema` 세 스키마 모두 코드에서 미참조 확인.
실제 서비스는 `auth_svc`, `chat_svc` 스키마를 각자 마이그레이션에서 생성.
주석에 실제 마이그레이션 경로 명시하여 의도 기록.

### 4. [infra] CMake 테스트 타겟 — 패턴 준수

**심각도**: Info
**상태**: 이슈 없음

기존 gateway 테스트 타겟 패턴 정확히 준수: `apex_set_warnings`, TIMEOUT 30초,
WIN32 분기, `apex_shared_adapters_redis` 링크 (jwt_blacklist.cpp의 RedisMultiplexer 의존).

### 5. [docs] CLAUDE.md 중복 정리 — 역할 분리 명확

**심각도**: Info
**상태**: 이슈 없음

루트 CLAUDE.md에서 docs/CLAUDE.md와 중복되던 3개 규칙 제거. 포인터만 유지.
docs/CLAUDE.md에 progress 문서 품질 요건 이관. 상호참조 문구 갱신.

## 요약

| 심각도 | 발견 | 수정 | 잔여 |
|--------|------|------|------|
| Critical | 0 | 0 | 0 |
| Major | 0 | 0 | 0 |
| Minor | 1 | 1 | 0 |
| Info | 4 | — | — |

**리뷰 이슈 잔여: 0건**
