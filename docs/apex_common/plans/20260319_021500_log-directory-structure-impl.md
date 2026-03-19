# #60 로그 디렉토리 구조 확립 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 로컬 파일 로깅을 서비스별/레벨별/날짜별로 구조화하고 async logger로 shared-nothing 준수

**Architecture:** `LogConfig` 확장 → `exact_level_sink` 래퍼 → `spdlog::async_logger` + `daily_file_format_sink` 조합. 프로젝트 루트 자동 탐지로 CWD 독립적 동작.

**Tech Stack:** spdlog (async_logger, daily_file_format_sink), toml++, std::filesystem

**Spec:** `docs/apex_common/plans/20260319_014826_log-directory-structure-design.md`

---

### Task 1: Config 구조체 + 파싱 변경 (config.hpp + config.cpp)

> Task 1과 2는 원자적으로 함께 수행 — struct 변경과 파싱 코드가 동시에 빌드되어야 컴파일 가능.

**Files:**
- Modify: `apex_core/include/apex/core/config.hpp:12-30`
- Modify: `apex_core/src/config.cpp:118-146`

- [ ] **Step 1: `LogFileConfig` 수정**

`path` 의미를 파일→디렉토리로 변경, `max_size_mb`/`max_files` 제거:

```cpp
struct LogFileConfig {
    bool enabled = false;
    std::string path;  // 빈 값 → {project_root}/logs/. 설정 시 그대로 사용
    bool json = false;
};
```

- [ ] **Step 2: `LogAsyncConfig` 신설**

```cpp
struct LogAsyncConfig {
    size_t queue_size = 8192;
};
```

- [ ] **Step 3: `LogConfig`에 `service_name` + `async` 추가**

```cpp
struct LogConfig {
    std::string level = "info";
    std::string framework_level = "info";
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v";
    std::string service_name;  // 빈 값 시 바이너리명 사용
    LogConsoleConfig console;
    LogFileConfig file;
    LogAsyncConfig async;
};
```

- [ ] **Step 4: `parse_logging()` 수정 (config.cpp)**

`service_name`, `[logging.async]` 파싱 추가. `max_size_mb`/`max_files` 파싱 제거 (존재해도 무시):

```cpp
LogConfig parse_logging(const toml::table& root) {
    LogConfig cfg;
    auto* tbl = root["logging"].as_table();
    if (!tbl) return cfg;

    cfg.level = get_or<std::string>(*tbl, "level", cfg.level);
    cfg.framework_level = get_or<std::string>(*tbl, "framework_level", cfg.framework_level);
    cfg.pattern = get_or<std::string>(*tbl, "pattern", cfg.pattern);
    cfg.service_name = get_or<std::string>(*tbl, "service_name", cfg.service_name);

    // [logging.console]
    if (auto* con = (*tbl)["console"].as_table()) {
        cfg.console.enabled = get_or<bool>(*con, "enabled", cfg.console.enabled);
    }

    // [logging.file]
    if (auto* file = (*tbl)["file"].as_table()) {
        cfg.file.enabled = get_or<bool>(*file, "enabled", cfg.file.enabled);
        cfg.file.path = get_or<std::string>(*file, "path", cfg.file.path);
        cfg.file.json = get_or<bool>(*file, "json", cfg.file.json);
        // deprecated: max_size_mb, max_files — 존재해도 무시 (spdlog 미초기화 상태이므로 경고 출력 안 함)
    }

    // [logging.async]
    if (auto* async_tbl = (*tbl)["async"].as_table()) {
        cfg.async.queue_size = checked_narrow<size_t>(
            get_or<int64_t>(*async_tbl, "queue_size", int64_t{8192}),
            "queue_size");
    }

    return cfg;
}
```

- [ ] **Step 5: 컴파일 확인**

Run: `"D:/.workspace/apex_pipeline_branch_01/apex_tools/queue-lock.sh" build debug --target apex_core`
Expected: config.hpp + config.cpp 컴파일 성공. 테스트 파일에서 아직 에러 가능 (제거된 필드 참조).

---

### Task 2: Config 테스트 업데이트 (test_config.cpp)

**Files:**
- Modify: `apex_core/tests/unit/test_config.cpp`

- [ ] **Step 1: 기존 테스트 수정**

`FromFileLoadsLoggingSection` 테스트에서 `max_size_mb`, `max_files`, `path`(파일경로) 관련 assertion 제거/변경:

```cpp
TEST_F(ConfigTest, FromFileLoadsLoggingSection) {
    auto path = write_toml("logging.toml", R"(
[logging]
level = "debug"
framework_level = "warn"
service_name = "test-svc"

[logging.file]
enabled = true
path = "/var/log/apex"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.level, "debug");
    EXPECT_EQ(config.logging.framework_level, "warn");
    EXPECT_EQ(config.logging.service_name, "test-svc");
    EXPECT_TRUE(config.logging.file.enabled);
    EXPECT_EQ(config.logging.file.path, "/var/log/apex");
    // json 기본값 false
    EXPECT_FALSE(config.logging.file.json);
}
```

`NegativeMaxSizeMbThrows` 테스트 제거 (필드 삭제됨).

- [ ] **Step 2: 새 테스트 추가 — service_name 및 async**

```cpp
TEST_F(ConfigTest, ServiceNameDefaultEmpty) {
    auto path = write_toml("no_svc.toml", R"(
[logging]
level = "info"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_TRUE(config.logging.service_name.empty());
}

TEST_F(ConfigTest, AsyncQueueSizeParsed) {
    auto path = write_toml("async.toml", R"(
[logging.async]
queue_size = 16384
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.async.queue_size, 16384);
}

TEST_F(ConfigTest, AsyncQueueSizeDefaultIs8192) {
    auto path = write_toml("no_async.toml", "");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.async.queue_size, 8192);
}

TEST_F(ConfigTest, DeprecatedFieldsIgnored) {
    auto path = write_toml("deprecated.toml", R"(
[logging.file]
enabled = true
max_size_mb = 100
max_files = 3
)");
    // deprecated 필드는 무시 — 에러 없이 파싱 성공
    EXPECT_NO_THROW(AppConfig::from_file(path));
}
```

- [ ] **Step 3: 빌드 + 테스트 실행**

Run: `"D:/.workspace/apex_pipeline_branch_01/apex_tools/queue-lock.sh" build debug --target apex_core_tests`
Run: `./build/Windows/debug/apex_core/tests/unit/apex_core_tests.exe --gtest_filter="ConfigTest.*"`
Expected: 전체 PASS

- [ ] **Step 4: 커밋**

```bash
git add apex_core/include/apex/core/config.hpp apex_core/src/config.cpp apex_core/tests/unit/test_config.cpp
git commit -m "feat(core): #60 LogConfig 구조체 변경 — service_name, AsyncConfig, path 의미 변경"
```

---

### Task 3: 프로젝트 루트 탐지 유틸리티

**Files:**
- Modify: `apex_core/src/logging.cpp`

- [ ] **Step 1: `find_project_root()` 구현**

logging.cpp의 anonymous namespace에 추가:

```cpp
/// 현재 실행 파일 위치에서 상위로 올라가며 .git 디렉토리를 찾아 프로젝트 루트 반환.
/// 찾지 못하면 현재 작업 디렉토리 반환.
std::filesystem::path find_project_root() {
    namespace fs = std::filesystem;
    // 현재 작업 디렉토리부터 탐색
    auto current = fs::current_path();
    auto search = current;
    while (true) {
        if (fs::exists(search / ".git")) {
            return search;
        }
        auto parent = search.parent_path();
        if (parent == search) break;  // 루트 도달
        search = parent;
    }
    return current;  // fallback: CWD
}

/// log_root 경로 resolve.
/// 빈 값 → {project_root}/logs/
/// 값 있음 → 그대로 사용 (상대/절대)
std::filesystem::path resolve_log_path(const std::string& path) {
    if (path.empty()) {
        return find_project_root() / "logs";
    }
    return std::filesystem::path(path);
}
```

- [ ] **Step 2: 컴파일 확인** (아직 호출하는 코드 없으므로 경고만 확인)

---

### Task 4: exact_level_sink 구현

**Files:**
- Modify: `apex_core/include/apex/core/logging.hpp`
- Modify: `apex_core/tests/unit/test_logging.cpp`

- [ ] **Step 1: test_logging.cpp에 exact_level_sink 테스트 작성**

```cpp
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>

TEST_F(LoggingTest, ExactLevelSinkFiltersExactLevel) {
    std::ostringstream oss;
    auto ostream_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto exact_info = std::make_shared<apex::core::exact_level_sink<std::mutex>>(
        ostream_sink, spdlog::level::info);

    auto logger = std::make_shared<spdlog::logger>("test_exact", exact_info);
    logger->set_level(spdlog::level::trace);

    logger->trace("trace msg");
    logger->debug("debug msg");
    logger->info("info msg");
    logger->warn("warn msg");
    logger->error("error msg");
    logger->flush();

    auto output = oss.str();
    EXPECT_NE(output.find("info msg"), std::string::npos);
    EXPECT_EQ(output.find("trace msg"), std::string::npos);
    EXPECT_EQ(output.find("debug msg"), std::string::npos);
    EXPECT_EQ(output.find("warn msg"), std::string::npos);
    EXPECT_EQ(output.find("error msg"), std::string::npos);
}
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Run: `./build/Windows/debug/apex_core/tests/unit/apex_core_tests.exe --gtest_filter="LoggingTest.ExactLevelSinkFiltersExactLevel"`
Expected: FAIL (exact_level_sink 미정의)

- [ ] **Step 3: logging.hpp에 exact_level_sink 구현**

```cpp
#pragma once

#include <apex/core/config.hpp>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>

namespace apex::core {

void init_logging(const LogConfig& config);
void shutdown_logging();

/// 정확히 해당 레벨의 로그만 통과시키는 필터 sink.
/// spdlog set_level()은 최소 레벨 필터이므로, 레벨별 파일 분리에 이 래퍼 사용.
template <typename Mutex>
class exact_level_sink : public spdlog::sinks::base_sink<Mutex> {
public:
    exact_level_sink(std::shared_ptr<spdlog::sinks::sink> inner,
                     spdlog::level::level_enum target)
        : inner_(std::move(inner)), target_(target) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (msg.level == target_) {
            inner_->log(msg);
        }
    }
    void flush_() override { inner_->flush(); }

private:
    std::shared_ptr<spdlog::sinks::sink> inner_;
    spdlog::level::level_enum target_;
};

} // namespace apex::core
```

- [ ] **Step 4: 테스트 실행 — 성공 확인**

Run: `./build/Windows/debug/apex_core/tests/unit/apex_core_tests.exe --gtest_filter="LoggingTest.ExactLevelSinkFiltersExactLevel"`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/logging.hpp apex_core/tests/unit/test_logging.cpp
git commit -m "feat(core): #60 exact_level_sink 구현 — 레벨별 파일 분리용 필터 sink"
```

---

### Task 5: init_logging() 재구현

**Files:**
- Modify: `apex_core/src/logging.cpp`
- Modify: `apex_core/tests/unit/test_logging.cpp`

- [ ] **Step 1: test_logging.cpp에 새 동작 테스트 작성**

```cpp
TEST_F(LoggingTest, FileEnabledCreatesPerLevelFiles) {
    auto tmp = std::filesystem::temp_directory_path() / "apex_log_level_test";
    std::filesystem::remove_all(tmp);  // 이전 테스트 잔여물 정리

    {
        LogConfig cfg;
        cfg.file.enabled = true;
        cfg.file.path = tmp.string();
        cfg.service_name = "test-svc";
        init_logging(cfg);

        auto logger = spdlog::get("app");
        ASSERT_NE(logger, nullptr);
        logger->set_level(spdlog::level::trace);
        logger->trace("trace test");
        logger->info("info test");
        logger->warn("warn test");
        logger->error("error test");
        logger->flush();

        // 서비스 디렉토리 생성 확인
        auto svc_dir = tmp / "test-svc";
        EXPECT_TRUE(std::filesystem::exists(svc_dir));
    }

    shutdown_logging();
    std::filesystem::remove_all(tmp);
}

TEST_F(LoggingTest, AsyncLoggerCreated) {
    LogConfig cfg;
    cfg.file.enabled = true;
    cfg.file.path = (std::filesystem::temp_directory_path() / "apex_async_test").string();
    cfg.service_name = "async-svc";
    init_logging(cfg);

    auto logger = spdlog::get("app");
    ASSERT_NE(logger, nullptr);
    // async logger는 sinks에 접근 가능
    EXPECT_GE(logger->sinks().size(), 6);  // 최소 6개 레벨별 파일 sink

    shutdown_logging();
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "apex_async_test");
}

TEST_F(LoggingTest, ServiceNameValidation) {
    LogConfig cfg;
    cfg.service_name = "../escape";
    cfg.file.enabled = true;
    cfg.file.path = (std::filesystem::temp_directory_path() / "apex_validation_test").string();
    EXPECT_THROW(init_logging(cfg), std::invalid_argument);
}
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Expected: FAIL (init_logging이 아직 old 구현)

- [ ] **Step 3: logging.cpp 재구현**

주요 변경:
- `#include <spdlog/async.h>` + `#include <spdlog/sinks/daily_file_format_sink.h>` 추가
- `rotating_file_sink` include 제거
- `validate_service_name()` 함수 추가
- `init_logging()` 전면 재작성:

```cpp
#include <apex/core/logging.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_format_sink.h>
#include <spdlog/pattern_formatter.h>

#include <array>
#include <filesystem>
#include <memory>
#include <regex>
#include <stdexcept>
#include <vector>

namespace apex::core {

namespace {

namespace fs = std::filesystem;

spdlog::level::level_enum parse_level(const std::string& level_str) {
    return spdlog::level::from_str(level_str);
}

// JsonFormatter 클래스 — 기존 코드 그대로 유지 (변경 없음)

fs::path find_project_root() { /* Task 4에서 구현 */ }
fs::path resolve_log_path(const std::string& path) { /* Task 4에서 구현 */ }

void validate_service_name(const std::string& name) {
    static const std::regex valid_pattern("^[a-z0-9_-]+$");
    if (name.find("..") != std::string::npos || name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos) {
        throw std::invalid_argument(
            "LogConfig: service_name contains path traversal characters: '" + name + "'");
    }
    if (!std::regex_match(name, valid_pattern)) {
        throw std::invalid_argument(
            "LogConfig: service_name must match [a-z0-9_-]+, got: '" + name + "'");
    }
}

/// service_name이 비어있으면 "default" 사용.
/// 모든 서비스 TOML에 service_name이 명시되므로 실제로는 fallback에 도달하지 않음.
std::string resolve_service_name(const std::string& name) {
    if (!name.empty()) return name;
    return "default";
}

} // anonymous namespace

void init_logging(const LogConfig& config) {
    // Step 0: 기존 로거 + thread pool 완전 정리
    spdlog::shutdown();

    std::vector<spdlog::sink_ptr> sinks;

    // ConsoleSink
    if (config.console.enabled) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(config.pattern);
        sinks.push_back(console_sink);
    }

    // FileSinks — 레벨별 6파일
    if (config.file.enabled) {
        auto svc_name = resolve_service_name(config.service_name);
        validate_service_name(svc_name);

        auto log_dir = resolve_log_path(config.file.path) / svc_name;
        fs::create_directories(log_dir);

        // 6개 레벨: trace, debug, info, warn, err, critical
        constexpr std::array levels = {
            spdlog::level::trace, spdlog::level::debug,
            spdlog::level::info,  spdlog::level::warn,
            spdlog::level::err,   spdlog::level::critical
        };
        constexpr std::array<const char*, 6> level_names = {
            "trace", "debug", "info", "warn", "error", "critical"
        };

        for (size_t i = 0; i < levels.size(); ++i) {
            auto pattern = (log_dir / (std::string("%Y%m%d_") + level_names[i] + ".log")).string();
            auto daily = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(
                pattern, 0, 0, false, 0);
            if (config.file.json) {
                daily->set_formatter(std::make_unique<JsonFormatter>());
            } else {
                daily->set_pattern(config.pattern);
            }
            auto exact = std::make_shared<exact_level_sink<std::mutex>>(daily, levels[i]);
            sinks.push_back(exact);
        }
    }

    // Async thread pool + logger 생성
    spdlog::init_thread_pool(config.async.queue_size, 1);

    auto apex_logger = std::make_shared<spdlog::async_logger>(
        "apex", sinks.begin(), sinks.end(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    apex_logger->set_level(parse_level(config.framework_level));
    spdlog::register_logger(apex_logger);

    auto app_logger = std::make_shared<spdlog::async_logger>(
        "app", sinks.begin(), sinks.end(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    app_logger->set_level(parse_level(config.level));
    spdlog::register_logger(app_logger);
}

void shutdown_logging() {
    spdlog::shutdown();
}

} // namespace apex::core
```

- [ ] **Step 4: 기존 test_logging.cpp 테스트 수정**

`FileEnabled` 테스트를 새 구조에 맞게 수정:

```cpp
TEST_F(LoggingTest, FileEnabled) {
    auto tmp = std::filesystem::temp_directory_path() / "apex_log_test";
    std::filesystem::remove_all(tmp);

    {
        LogConfig cfg;
        cfg.file.enabled = true;
        cfg.file.path = tmp.string();
        cfg.service_name = "test-svc";
        init_logging(cfg);

        auto logger = spdlog::get("app");
        // Console(1) + 6 level sinks = 7
        EXPECT_EQ(logger->sinks().size(), 7);

        logger->info("test message");
        logger->flush();
    }

    shutdown_logging();
    std::filesystem::remove_all(tmp);
}
```

`ConsoleOnlyByDefault` — 변경 불필요 (file.enabled = false 기본값)

- [ ] **Step 5: 빌드 + 전체 로깅 테스트 실행**

Run: `"D:/.workspace/apex_pipeline_branch_01/apex_tools/queue-lock.sh" build debug --target apex_core_tests`
Run: `./build/Windows/debug/apex_core/tests/unit/apex_core_tests.exe --gtest_filter="LoggingTest.*"`
Expected: 전체 PASS

- [ ] **Step 6: 커밋**

```bash
git add apex_core/src/logging.cpp apex_core/tests/unit/test_logging.cpp
git commit -m "feat(core): #60 init_logging() 재구현 — async logger + 레벨별 daily file sink"
```

---

### Task 6: default.toml 업데이트

**Files:**
- Modify: `apex_core/config/default.toml`

- [ ] **Step 1: default.toml 갱신**

```toml
[logging]
level = "info"
framework_level = "info"
pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v"
# service_name — 서비스별 TOML에서 설정. 미설정 시 "default"

[logging.console]
enabled = true

[logging.file]
enabled = false
path = ""                      # 빈 값 → {project_root}/logs/. 설정 시 그대로 사용
json = false

[logging.async]
queue_size = 8192
```

- [ ] **Step 2: 커밋**

```bash
git add apex_core/config/default.toml
git commit -m "feat(core): #60 default.toml 로깅 설정 갱신"
```

---

### Task 7: 서비스 TOML에 [logging] 섹션 추가

**Files:**
- Modify: `apex_services/gateway/gateway.toml`
- Modify: `apex_services/auth-svc/auth_svc.toml`
- Modify: `apex_services/chat-svc/chat_svc.toml`

- [ ] **Step 1: gateway.toml에 추가**

```toml
[logging]
service_name = "gateway"

[logging.file]
enabled = true
```

- [ ] **Step 2: auth_svc.toml에 추가**

```toml
[logging]
service_name = "auth-svc"

[logging.file]
enabled = true
```

- [ ] **Step 3: chat_svc.toml에 추가**

```toml
[logging]
service_name = "chat-svc"

[logging.file]
enabled = true
```

- [ ] **Step 4: 커밋**

```bash
git add apex_services/gateway/gateway.toml apex_services/auth-svc/auth_svc.toml apex_services/chat-svc/chat_svc.toml
git commit -m "feat(services): #60 서비스 TOML에 [logging] 섹션 추가"
```

---

### Task 8: 서비스 main.cpp에 init_logging() 통합

**Files:**
- Modify: `apex_services/gateway/src/main.cpp`
- Modify: `apex_services/auth-svc/src/main.cpp`
- Modify: `apex_services/chat-svc/src/main.cpp`

- [ ] **Step 1: Gateway main.cpp 수정**

기존 `spdlog::set_level(spdlog::level::info)` 를 제거하고 `init_logging()` 호출 추가.
각 서비스의 TOML 파싱 시점 이후, 서버 시작 전에 삽입:

```cpp
#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>

// config 파싱 직후:
auto log_config = apex::core::AppConfig::from_file(config_path).logging;
apex::core::init_logging(log_config);
```

Note: 각 서비스가 자체 config 구조체를 쓰므로, `AppConfig::from_file()`은 로깅 설정 추출용으로만 호출. unknown 섹션은 자동 무시.

- [ ] **Step 2: Auth main.cpp 수정**

동일 패턴. `spdlog::set_level(spdlog::level::info)` 제거 → `init_logging()` 호출.

- [ ] **Step 3: Chat main.cpp 수정**

동일 패턴. `spdlog::set_level()` 없지만 `init_logging()` 호출 추가.

- [ ] **Step 4: 빌드 확인**

Run: `"D:/.workspace/apex_pipeline_branch_01/apex_tools/queue-lock.sh" build debug`
Expected: 전체 빌드 성공

- [ ] **Step 5: 커밋**

```bash
git add apex_services/gateway/src/main.cpp apex_services/auth-svc/src/main.cpp apex_services/chat-svc/src/main.cpp
git commit -m "feat(services): #60 서비스 main.cpp에 init_logging() 통합"
```

---

### Task 9: E2E TOML 업데이트 + fixture 경로 변경

**Files:**
- Modify: `apex_services/tests/e2e/gateway_e2e.toml`
- Modify: `apex_services/tests/e2e/auth_svc_e2e.toml`
- Modify: `apex_services/tests/e2e/chat_svc_e2e.toml`
- Modify: `apex_services/tests/e2e/e2e_test_fixture.cpp:40-53`

- [ ] **Step 1: E2E TOML에 [logging] 섹션 추가**

각 E2E TOML에:
```toml
[logging]
service_name = "gateway"  # / "auth-svc" / "chat-svc"

[logging.file]
enabled = true
```

- [ ] **Step 2: e2e_test_fixture.cpp 수정 — 로그 경로 변경**

`launch_service()` 함수에서 로그 파일 경로를 변경:

```cpp
// 변경 전 (라인 50-52):
std::string log_file_path = name + "_e2e.log";
if (!config_.project_root.empty()) {
    log_file_path = config_.project_root + "/" + log_file_path;
}

// 변경 후:
// service name 매핑 (프로세스명 → TOML service_name)
auto svc_name = name;  // "Gateway" → "gateway" 등 변환 필요
std::transform(svc_name.begin(), svc_name.end(), svc_name.begin(), ::tolower);
// "authservice" → "auth-svc", "chatservice" → "chat-svc" 매핑
if (svc_name == "authservice") svc_name = "auth-svc";
else if (svc_name == "chatservice") svc_name = "chat-svc";

// 날짜 접두사
auto now = std::chrono::system_clock::now();
auto tt = std::chrono::system_clock::to_time_t(now);
std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
char date_buf[9];
std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm);

std::string log_dir = "logs/" + svc_name;
if (!config_.project_root.empty()) {
    log_dir = config_.project_root + "/" + log_dir;
}
std::filesystem::create_directories(log_dir);

std::string log_file_path = log_dir + "/" + std::string(date_buf) + "_e2e.log";
```

Note: E2E fixture 호출 시 name 인자 값 확인 필요 — `SetUpTestSuite()`에서 확인. 현재 "Gateway", "AuthService", "ChatService"로 호출.

- [ ] **Step 3: 빌드 확인**

Run: `"D:/.workspace/apex_pipeline_branch_01/apex_tools/queue-lock.sh" build debug`
Expected: 전체 빌드 성공

- [ ] **Step 4: E2E 테스트 실행** (Docker 인프라 기동 상태에서)

Run: `./build/Windows/debug/apex_services/tests/e2e/apex_e2e_tests.exe`
Expected: 11/11 PASS, 로그가 `logs/{service}/` 디렉토리에 생성됨

- [ ] **Step 5: 커밋**

```bash
git add apex_services/tests/e2e/
git commit -m "feat(e2e): #60 E2E 로그 경로를 logs/{service}/ 구조로 이동"
```

---

### Task 10: 백로그 + 문서 갱신

**Files:**
- Modify: `docs/BACKLOG.md`
- Modify: `docs/BACKLOG_HISTORY.md`
- Modify: `docs/Apex_Pipeline.md`
- Modify: `CLAUDE.md` (루트)

- [ ] **Step 1: BACKLOG.md에서 #60 삭제 + 보존 정책 항목 추가**

#60을 NOW에서 삭제. 새 항목 추가 (다음 발번 사용):
```markdown
### #{next_id}. 로그 보존 정책 TOML 파라미터화
- **등급**: MINOR
- **스코프**: core
- **타입**: infra
- **설명**: `retention_days` 등으로 자동 삭제 제어. 현재 영구 보존. 디스크 용량 이슈 발생 시 트리거.
```

- [ ] **Step 2: BACKLOG_HISTORY.md에 #60 완료 기록**

```markdown
### #60. 로그 디렉토리 구조 확립 + 경로 중앙화 + 파일명 표준화
- **등급**: MAJOR | **스코프**: core, gateway, auth-svc, chat-svc, infra | **타입**: infra
- **해결**: {현재 시각} | **방식**: FIXED | **커밋**: {short hash}
- **비고**: async logger + daily_file_format_sink + exact_level_sink 조합. 서비스별/레벨별/날짜별 로그 분리 구현
```

- [ ] **Step 3: Apex_Pipeline.md 로드맵 갱신**

v0.5.5.1 이후에 v0.5.5.2 항목 추가 (또는 적절한 버전):
```
| v0.5.5.2 | 소 | 로그 디렉토리 구조 확립 — 서비스별/레벨별 async 파일 로깅, 프로젝트 루트 자동 탐지 | 완료 |
```

- [ ] **Step 4: CLAUDE.md 로드맵 버전 갱신**

현재 버전을 v0.5.5.2로 업데이트.

- [ ] **Step 5: 커밋**

```bash
git add docs/ CLAUDE.md
git commit -m "docs: #60 완료 — 백로그 정리 + 로드맵 갱신"
```
