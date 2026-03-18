# #60. 로그 디렉토리 구조 확립 + 경로 중앙화 + 파일명 표준화

## 배경

로그 파일이 프로젝트 루트에 산란하는 문제. E2E fixture가 `{name}_e2e.log`를 루트에 직접 생성하고, 서비스들은 `init_logging()`을 호출하지 않아 파일 로깅 자체가 비활성 상태.
이번 작업은 **dev/debug용 로컬 파일 로깅** 정비가 스코프. 운영 중앙 로깅은 기존 KafkaSink 활용 (스코프 밖).

## 디렉토리 구조

```
{path}/{service_name}/
  {YYYYMMDD}_{level}.log
```

예시:
```
logs/
  gateway/
    20260319_trace.log
    20260319_debug.log
    20260319_info.log
    20260319_warn.log
    20260319_error.log
    20260319_critical.log
  auth-svc/
    20260319_info.log
    ...
  chat-svc/
    20260319_info.log
    ...
```

- 서비스별 1단계 분리. 환경별/날짜별 추가 계층 없음
- 날짜는 파일명 접두사 (하이픈 없는 `YYYYMMDD`)
- 레벨별 6파일 (trace/debug/info/warn/error/critical), 각 파일에 해당 레벨만 출력
- 보존 정책: 영구 (자동 삭제 없음)

## TOML 설정 스키마

```toml
[logging]
level = "info"
framework_level = "info"
pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v"
service_name = "gateway"        # → logs/gateway/

[logging.console]
enabled = true

[logging.file]
enabled = true
path = ""                   # 빈 값/미설정 → {project_root}/logs/ (기본값). 값 설정 시 상대/절대 경로 그대로 사용
json = false                    # JSON 포맷 (기본: 텍스트)

[logging.async]
queue_size = 8192               # async 큐 크기 (2의 거듭제곱)
```

변경 포인트:
- `service_name` 신설 — 로그 디렉토리명 결정. 검증 규칙: `[a-z0-9_-]+` (영소문자, 숫자, 언더스코어, 하이픈만 허용). `..` 또는 `/` 포함 시 reject (경로 순회 방지). 미설정 시 서비스 실행 바이너리명 사용
- `path` 의미 변경 — **경로 해석 규칙**: 빈 값/미설정 → `{project_root}/logs/` (git root 탐지, 안전한 기본값). 값 설정 시 상대/절대 경로 그대로 사용 (사용자 의도 존중)
- `[logging.async]` 신설 — async logger 큐 크기 설정
- 기존 `max_size_mb`, `max_files` 제거 — daily 기반으로 전환. 기존 TOML에 이 필드가 남아 있으면 파싱 시 무시 (경고 로그 출력)
- `thread_count`(1 고정), `overflow_policy`(`overrun_oldest` 고정)는 코드 내 상수
- `json` 옵션은 유지 (기본값 `false` = 텍스트). 기존 `JsonFormatter` 그대로 활용

## 로거 아키텍처

### shared-nothing 준수 설계

```
코어0 ──enqueue(나노초)──┐
코어1 ──enqueue(나노초)──┤──→ [mpmc queue] ──→ 백그라운드 스레드(1) ──→ 파일 I/O
코어2 ──enqueue(나노초)──┘
```

- `spdlog::async_logger` 사용 — 코어 스레드는 큐에 enqueue만 수행 (나노초)
- 백그라운드 스레드 1개가 실제 파일 I/O 담당
- overflow 정책: `overrun_oldest` — 큐 풀 시 코어 스레드 블로킹 없음, 가장 오래된 메시지 덮어씀
- dev/debug 환경에서 백그라운드 스레드 컨텍스트 스위칭은 실질적 영향 없음 (개발 머신 여유 코어 활용)
- 운영 환경에서는 KafkaSink가 중앙 로깅 담당, 로컬 파일 로깅 비활성화 가능

### exact_level_sink

spdlog의 `set_level()`은 최소 레벨 필터(해당 레벨 이상 전부). 정확히 해당 레벨만 출력하려면 래퍼 sink 필요.

```cpp
template <typename Mutex>
class exact_level_sink : public spdlog::sinks::base_sink<Mutex> {
    std::shared_ptr<spdlog::sinks::sink> inner_;
    spdlog::level::level_enum target_;
public:
    exact_level_sink(std::shared_ptr<spdlog::sinks::sink> inner,
                     spdlog::level::level_enum target)
        : inner_(std::move(inner)), target_(target) {}
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level == target_) inner_->log(msg);
    }
    void flush_() override { inner_->flush(); }
};
```

### init_logging() 핵심 흐름

0. `spdlog::shutdown()` — 기존 로거 + thread pool 완전 정리 (재초기화 안전 보장). 최초 호출 시에도 안전 (no-op)
1. `spdlog::init_thread_pool(queue_size, 1)` — async thread pool 생성
2. `fs::create_directories(path / service_name)` — 디렉토리 자동 생성. 실패 시 `std::runtime_error` throw (설정 오류 조기 발견)
3. 콘솔 sink 생성 (기존 유지)
4. 레벨별 6개 `exact_level_sink<daily_file_format_sink>` 조합 생성. 각 exact_level_sink이 해당 레벨의 daily sink을 래핑
5. `spdlog::async_logger` 생성 ("apex" + "app" 2개, overflow_policy: `overrun_oldest`)
6. `spdlog::register_logger()` 등록

## E2E 로그 처리

E2E fixture의 프로세스 stdout/stderr 리다이렉트도 같은 디렉토리 구조로 통합.
**E2E 로그는 spdlog 레벨 분리 대상이 아님** — OS 레벨 프로세스 출력(stdout/stderr)이므로 단일 `_e2e.log` 파일에 기록.

```
# 변경 전
{project_root}/Gateway_e2e.log

# 변경 후
{project_root}/logs/gateway/20260319_e2e.log
```

- E2E TOML에 `[logging]` 섹션 추가. 예시 (`gateway_e2e.toml`):
  ```toml
  [logging]
  service_name = "gateway"
  [logging.file]
  enabled = true
  path = "logs"
  ```
- fixture `launch_service()`에서 `logs/{service_name}/` 경로로 변경
- 파일명: `{YYYYMMDD}_e2e.log` (날짜 접두사 동일 형식, 레벨 분리 없음)
- 디렉토리 생성: `init_logging()`이 먼저 생성, fixture는 이미 있으면 skip

## 서비스 main.cpp 통합

현재 Gateway, Auth, Chat 서비스 main.cpp에서 `spdlog::set_level()`만 호출 중. `init_logging()` 호출로 통합.

```cpp
// 변경 전
spdlog::set_level(spdlog::level::info);

// 변경 후
auto app_config = AppConfig::from_file("gateway.toml");
init_logging(app_config.logging);
```

shutdown 경로: 기존 `shutdown_logging()` → `spdlog::shutdown()` 호출로 async thread pool 포함 정리. 변경 불필요.

## 변경 파일 목록

**수정:**

| 파일 | 변경 내용 |
|------|----------|
| `apex_core/include/apex/core/config.hpp` | `LogFileConfig`의 `path` 의미 변경 (파일→디렉토리), `max_size_mb`/`max_files` 제거, `AsyncConfig` 신설, `LogConfig`에 `service_name` 추가 |
| `apex_core/src/config.cpp` | TOML 파싱에 새 필드 반영 |
| `apex_core/config/default.toml` | 기본 설정 갱신 |
| `apex_core/include/apex/core/logging.hpp` | `exact_level_sink` 정의 추가 |
| `apex_core/src/logging.cpp` | `init_logging()` 재구현 — async + daily_file_format + exact_level 조합 |
| `apex_services/gateway/gateway.toml` | `[logging]` 섹션 추가 |
| `apex_services/auth-svc/auth_svc.toml` | `[logging]` 섹션 추가 |
| `apex_services/chat-svc/chat_svc.toml` | `[logging]` 섹션 추가 |
| `apex_services/tests/e2e/gateway_e2e.toml` | `[logging]` 섹션 추가 |
| `apex_services/tests/e2e/auth_svc_e2e.toml` | `[logging]` 섹션 추가 |
| `apex_services/tests/e2e/chat_svc_e2e.toml` | `[logging]` 섹션 추가 |
| `apex_services/tests/e2e/e2e_test_fixture.cpp` | E2E 로그 경로를 `logs/{service}/` 구조로 변경 |
| `apex_core/tests/unit/test_logging.cpp` | 새 구조에 맞게 테스트 수정/추가 |
| 서비스 main.cpp 3개 | `init_logging()` 호출 추가 |

**신규 파일:** 없음

## 백로그 추가 항목

- 로그 보존 정책 TOML 파라미터화 (`retention_days` 등) — 디스크 용량 이슈 발생 시 트리거
