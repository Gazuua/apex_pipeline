# #60 로그 디렉토리 구조 확립 — 완료 기록

**백로그**: #60 | **브랜치**: feature/60-log-directory-structure | **버전**: v0.5.5.2

## 결과 요약

로컬 파일 로깅을 서비스별/레벨별/날짜별로 구조화하고 async logger로 shared-nothing을 준수하는 로깅 시스템 구현 완료.

### 구현 내용

1. **Config 구조체 변경** — `LogFileConfig`에서 `max_size_mb`/`max_files` 제거, `path` 의미를 파일→디렉토리로 변경. `LogAsyncConfig` 신설, `LogConfig`에 `service_name` + `async` 추가.

2. **exact_level_sink** — spdlog의 최소 레벨 필터 대신 정확히 해당 레벨만 통과시키는 래퍼 sink. 레벨별 파일 분리의 핵심 컴포넌트.

3. **init_logging() 재구현** — `spdlog::async_logger` + `daily_file_format_sink_mt` + `exact_level_sink` 조합. 6개 레벨별 daily file sink 생성. `overrun_oldest` overflow 정책으로 코어 스레드 블로킹 없음.

4. **프로젝트 루트 자동 탐지** — `find_project_root()`가 CWD에서 `.git` 디렉토리를 찾아 올라감. `path` 빈 값 시 `{project_root}/logs/` 사용.

5. **서비스 TOML 통합** — Gateway, Auth, Chat 서비스 + E2E TOML에 `[logging]` 섹션 추가. 각 서비스 main.cpp에서 `init_logging()` 호출.

6. **E2E fixture 경로 변경** — `{name}_e2e.log` (루트 산란) → `logs/{service}/YYYYMMDD_e2e.log` (구조화된 디렉토리).

### 로그 디렉토리 구조

```
logs/
  gateway/
    20260319_trace.log
    20260319_debug.log
    20260319_info.log
    ...
  auth-svc/
    20260319_info.log
    ...
  chat-svc/
    20260319_info.log
    ...
```

### 변경 파일 (14개)

| 파일 | 변경 |
|------|------|
| `apex_core/include/apex/core/config.hpp` | LogFileConfig, LogAsyncConfig, LogConfig 구조체 변경 |
| `apex_core/src/config.cpp` | parse_logging() — service_name, async 파싱, deprecated 필드 무시 |
| `apex_core/include/apex/core/logging.hpp` | exact_level_sink 템플릿 추가 |
| `apex_core/src/logging.cpp` | init_logging() 전면 재작성 — async + daily + exact_level |
| `apex_core/config/default.toml` | 새 로깅 설정 스키마 반영 |
| `apex_core/tests/unit/test_config.cpp` | 기존 테스트 수정 + 4개 신규 (service_name, async, deprecated) |
| `apex_core/tests/unit/test_logging.cpp` | 기존 테스트 수정 + 4개 신규 (exact_level, per-level, async, validation) |
| `apex_services/gateway/gateway.toml` | [logging] 섹션 추가 |
| `apex_services/auth-svc/auth_svc.toml` | [logging] 섹션 추가 |
| `apex_services/chat-svc/chat_svc.toml` | [logging] 섹션 추가 |
| `apex_services/gateway/src/main.cpp` | init_logging() 호출 추가 |
| `apex_services/auth-svc/src/main.cpp` | init_logging() 호출 추가 |
| `apex_services/chat-svc/src/main.cpp` | init_logging() 호출 추가 |
| `apex_services/tests/e2e/e2e_test_fixture.cpp` | E2E 로그 경로 구조화 |
| E2E TOML 3개 | [logging] 섹션 추가 |

### 테스트

- 유닛 테스트: 71/71 PASS (Config 4개 신규 + Logging 4개 신규)
- `test_gateway_file_watcher` 1건 간헐 실패 — 기존 타이밍 이슈, 이번 변경과 무관
