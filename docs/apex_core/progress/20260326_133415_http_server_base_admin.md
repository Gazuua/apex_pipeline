# HttpServerBase 추출 + AdminHttpServer + 런타임 로그 레벨 — Progress

**날짜**: 2026-03-26
**브랜치**: `feature/log-level-and-log-svc`
**백로그**: BACKLOG-179

## 작업 요약

MetricsHttpServer에서 공통 HTTP 서버 로직을 HttpServerBase로 추출하고, AdminHttpServer를 신규 구현하여 런타임 로그 레벨 동적 전환 기능을 제공.

## 결과

### 신규 파일
- `apex_core/include/apex/core/http_server_base.hpp` — HTTP 서버 베이스 클래스
- `apex_core/src/http_server_base.cpp` — accept loop, session tracking, HTTP 파싱/응답
- `apex_core/include/apex/core/admin_http_server.hpp` — Admin HTTP 서버 (런타임 관리)
- `apex_core/src/admin_http_server.cpp` — GET/POST /admin/log-level 구현
- `apex_core/tests/unit/test_admin_http_server.cpp` — 9개 테스트 케이스

### 수정 파일
- `metrics_http_server.hpp/cpp` — HttpServerBase 상속으로 리팩터
- `server_config.hpp` — AdminConfig 추가
- `server.hpp` — AdminHttpServer 멤버 추가
- `server.cpp` — admin server 시작/정지 통합

### API
- `GET /admin/log-level?logger=apex` — 현재 로그 레벨 조회
- `POST /admin/log-level?logger=apex&level=debug` — 런타임 레벨 변경
- spdlog `set_level()` 사용 (thread-safe)

## 빌드/테스트
- MSVC debug: 95/95 통과
