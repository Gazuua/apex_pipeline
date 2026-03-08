# Phase 5 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** apex_core 프레임워크에 TOML 설정, spdlog 로깅, Graceful Shutdown 타임아웃, CI/CD를 추가한다.

**Architecture:** AppConfig가 TOML 파일을 파싱하여 ServerConfig + LogConfig를 생성한다. init_logging()이 LogConfig 기반으로 spdlog 로거 2개(apex, app)를 초기화한다. Server::poll_shutdown()에 drain_timeout 기반 deadline을 추가한다. GitHub Actions로 4잡 매트릭스(MSVC, GCC, ASAN, TSAN) CI를 구성한다.

**Tech Stack:** C++23, toml++ (header-only), spdlog, CMake + Ninja, vcpkg, GitHub Actions

**설계서:** `docs/apex_core/plans/20260308_070528_phase5-design.md`

---

## Task 1: vcpkg + CMake 의존성 추가

**Files:**
- Modify: `apex_core/vcpkg.json`
- Modify: `vcpkg.json` (루트)
- Modify: `apex_core/CMakeLists.txt`

**Step 1: vcpkg.json에 spdlog, tomlplusplus 추가**

`apex_core/vcpkg.json`:
```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "apex-core",
  "version-semver": "0.2.4",
  "description": "Apex Core - High-performance real-time server framework",
  "builtin-baseline": "b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01",
  "dependencies": [
    { "name": "boost-asio", "version>=": "1.84.0" },
    { "name": "boost-beast", "version>=": "1.84.0" },
    "flatbuffers",
    "gtest",
    "spdlog",
    "tomlplusplus"
  ]
}
```

루트 `vcpkg.json`도 동일하게 spdlog, tomlplusplus 추가.

**Step 2: CMakeLists.txt에 find_package + link 추가**

`apex_core/CMakeLists.txt`에 추가:
```cmake
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
```

`target_link_libraries`에 추가:
```cmake
target_link_libraries(apex_core
    PUBLIC
        Boost::boost
        flatbuffers::flatbuffers
        spdlog::spdlog
        tomlplusplus::tomlplusplus
)
```

**Step 3: 빌드 검증**

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 빌드 성공, 기존 테스트 전부 통과

**Step 4: Commit**

```bash
git add apex_core/vcpkg.json vcpkg.json apex_core/CMakeLists.txt
git commit -m "build: spdlog, tomlplusplus 의존성 추가"
```

---

## Task 2: Config 구조체 + TOML 파싱 (TDD)

**Files:**
- Create: `apex_core/include/apex/core/config.hpp`
- Create: `apex_core/src/config.cpp`
- Create: `apex_core/config/default.toml`
- Create: `apex_core/tests/unit/test_config.cpp`
- Modify: `apex_core/include/apex/core/server.hpp` (ServerConfig에 drain_timeout 추가)
- Modify: `apex_core/CMakeLists.txt` (src/config.cpp 추가)
- Modify: `apex_core/tests/unit/CMakeLists.txt` (테스트 추가)

### Step 1: ServerConfig에 drain_timeout 필드 추가

`apex_core/include/apex/core/server.hpp` — ServerConfig에 추가:
```cpp
// Lifecycle
bool handle_signals = true;
std::chrono::seconds drain_timeout{25};  // Graceful Shutdown drain 타임아웃
```

### Step 2: config.hpp 헤더 작성

```cpp
// include/apex/core/config.hpp
#pragma once

#include <apex/core/server.hpp>
#include <string>

namespace apex::core {

struct LogConsoleConfig {
    bool enabled = true;
};

struct LogFileConfig {
    bool enabled = false;
    std::string path = "logs/apex.log";
    size_t max_size_mb = 100;
    size_t max_files = 3;
    bool json = true;
};

struct LogConfig {
    std::string level = "info";
    std::string framework_level = "info";
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v";
    LogConsoleConfig console;
    LogFileConfig file;
};

struct AppConfig {
    ServerConfig server;
    LogConfig logging;

    /// TOML 파일에서 설정 로딩. 파일 없으면 std::runtime_error.
    static AppConfig from_file(const std::string& path);

    /// 기본값으로 생성 (TOML 파일 불필요).
    static AppConfig defaults();
};

} // namespace apex::core
```

### Step 3: 실패하는 테스트 작성

`apex_core/tests/unit/test_config.cpp`:
```cpp
#include <apex/core/config.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

using namespace apex::core;

namespace {

// 테스트용 임시 TOML 파일 생성 헬퍼
class ConfigTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "apex_config_test";
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::string write_toml(const std::string& filename, const std::string& content) {
        auto path = tmp_dir_ / filename;
        std::ofstream ofs(path);
        ofs << content;
        return path.string();
    }
};

} // anonymous namespace

TEST_F(ConfigTest, DefaultsReturnsValidConfig) {
    auto config = AppConfig::defaults();
    EXPECT_EQ(config.server.port, 9000);
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.server.drain_timeout.count(), 25);
    EXPECT_EQ(config.logging.level, "info");
    EXPECT_TRUE(config.logging.console.enabled);
    EXPECT_FALSE(config.logging.file.enabled);
}

TEST_F(ConfigTest, FromFileLoadsServerSection) {
    auto path = write_toml("server.toml", R"(
[server]
port = 8080
num_cores = 4
drain_timeout_s = 10
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 8080);
    EXPECT_EQ(config.server.num_cores, 4);
    EXPECT_EQ(config.server.drain_timeout.count(), 10);
}

TEST_F(ConfigTest, FromFileLoadsLoggingSection) {
    auto path = write_toml("logging.toml", R"(
[logging]
level = "debug"
framework_level = "warn"

[logging.file]
enabled = true
path = "/var/log/apex.log"
max_size_mb = 50
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.logging.level, "debug");
    EXPECT_EQ(config.logging.framework_level, "warn");
    EXPECT_TRUE(config.logging.file.enabled);
    EXPECT_EQ(config.logging.file.path, "/var/log/apex.log");
    EXPECT_EQ(config.logging.file.max_size_mb, 50);
    // 누락 필드는 기본값
    EXPECT_EQ(config.logging.file.max_files, 3);
    EXPECT_TRUE(config.logging.file.json);
}

TEST_F(ConfigTest, MissingFieldsUseDefaults) {
    auto path = write_toml("minimal.toml", R"(
[server]
port = 7777
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 7777);
    // 나머지는 기본값
    EXPECT_EQ(config.server.num_cores, 1);
    EXPECT_EQ(config.server.drain_timeout.count(), 25);
    EXPECT_EQ(config.logging.level, "info");
}

TEST_F(ConfigTest, FileNotFoundThrows) {
    EXPECT_THROW(AppConfig::from_file("/nonexistent/path.toml"), std::runtime_error);
}

TEST_F(ConfigTest, InvalidTomlSyntaxThrows) {
    auto path = write_toml("bad.toml", "[[[[invalid toml");
    EXPECT_THROW(AppConfig::from_file(path), std::runtime_error);
}

TEST_F(ConfigTest, WrongFieldTypeThrows) {
    auto path = write_toml("badtype.toml", R"(
[server]
port = "not_a_number"
)");
    EXPECT_THROW(AppConfig::from_file(path), std::invalid_argument);
}

TEST_F(ConfigTest, UnknownSectionsIgnored) {
    auto path = write_toml("extra.toml", R"(
[server]
port = 9000

[kafka]
brokers = "localhost:9092"

[redis]
host = "localhost"
)");
    auto config = AppConfig::from_file(path);
    EXPECT_EQ(config.server.port, 9000);
    // kafka, redis 섹션은 무시 — 에러 없음
}
```

### Step 4: 테스트 실행 — 실패 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 컴파일 에러 (`from_file`, `defaults` 미구현)

### Step 5: config.cpp 구현

```cpp
// src/config.cpp
#include <apex/core/config.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <stdexcept>

namespace apex::core {

namespace {

template <typename T>
T get_or(const toml::table& tbl, std::string_view key, const T& default_val) {
    auto node = tbl[key];
    if (!node) return default_val;
    auto val = node.value<T>();
    if (!val) {
        throw std::invalid_argument(
            std::string("Config: invalid type for key '") + std::string(key) + "'");
    }
    return *val;
}

ServerConfig parse_server(const toml::table& root) {
    ServerConfig cfg;
    auto* tbl = root["server"].as_table();
    if (!tbl) return cfg;

    cfg.port = static_cast<uint16_t>(get_or<int64_t>(*tbl, "port", cfg.port));
    cfg.num_cores = static_cast<uint32_t>(get_or<int64_t>(*tbl, "num_cores", cfg.num_cores));
    cfg.mpsc_queue_capacity = static_cast<size_t>(
        get_or<int64_t>(*tbl, "mpsc_queue_capacity", static_cast<int64_t>(cfg.mpsc_queue_capacity)));
    cfg.drain_interval = std::chrono::microseconds(
        get_or<int64_t>(*tbl, "drain_interval_us", cfg.drain_interval.count()));
    cfg.heartbeat_timeout_ticks = static_cast<uint32_t>(
        get_or<int64_t>(*tbl, "heartbeat_timeout_ticks", cfg.heartbeat_timeout_ticks));
    cfg.recv_buf_capacity = static_cast<size_t>(
        get_or<int64_t>(*tbl, "recv_buf_capacity", static_cast<int64_t>(cfg.recv_buf_capacity)));
    cfg.timer_wheel_slots = static_cast<size_t>(
        get_or<int64_t>(*tbl, "timer_wheel_slots", static_cast<int64_t>(cfg.timer_wheel_slots)));
    cfg.handle_signals = get_or<bool>(*tbl, "handle_signals", cfg.handle_signals);
    cfg.drain_timeout = std::chrono::seconds(
        get_or<int64_t>(*tbl, "drain_timeout_s", cfg.drain_timeout.count()));

    return cfg;
}

LogConfig parse_logging(const toml::table& root) {
    LogConfig cfg;
    auto* tbl = root["logging"].as_table();
    if (!tbl) return cfg;

    cfg.level = get_or<std::string>(*tbl, "level", cfg.level);
    cfg.framework_level = get_or<std::string>(*tbl, "framework_level", cfg.framework_level);
    cfg.pattern = get_or<std::string>(*tbl, "pattern", cfg.pattern);

    // [logging.console]
    if (auto* con = (*tbl)["console"].as_table()) {
        cfg.console.enabled = get_or<bool>(*con, "enabled", cfg.console.enabled);
    }

    // [logging.file]
    if (auto* file = (*tbl)["file"].as_table()) {
        cfg.file.enabled = get_or<bool>(*file, "enabled", cfg.file.enabled);
        cfg.file.path = get_or<std::string>(*file, "path", cfg.file.path);
        cfg.file.max_size_mb = static_cast<size_t>(
            get_or<int64_t>(*file, "max_size_mb", static_cast<int64_t>(cfg.file.max_size_mb)));
        cfg.file.max_files = static_cast<size_t>(
            get_or<int64_t>(*file, "max_files", static_cast<int64_t>(cfg.file.max_files)));
        cfg.file.json = get_or<bool>(*file, "json", cfg.file.json);
    }

    return cfg;
}

} // anonymous namespace

AppConfig AppConfig::from_file(const std::string& path) {
    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(
            std::string("Failed to parse TOML config '") + path + "': " + std::string(e.description()));
    }

    AppConfig config;
    config.server = parse_server(tbl);
    config.logging = parse_logging(tbl);
    return config;
}

AppConfig AppConfig::defaults() {
    return AppConfig{};
}

} // namespace apex::core
```

### Step 6: CMakeLists.txt에 소스 + 테스트 등록

`apex_core/CMakeLists.txt` — `add_library(apex_core STATIC ...)` 목록에 `src/config.cpp` 추가.

`apex_core/tests/unit/CMakeLists.txt` — 끝에 추가:
```cmake
apex_add_unit_test(test_config test_config.cpp)
```

### Step 7: default.toml 작성

`apex_core/config/default.toml` — 설계서 §2.1의 내용 그대로 작성.

### Step 8: 빌드 + 테스트 통과 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 전체 테스트 통과 (기존 + test_config)

### Step 9: Commit

```bash
git add apex_core/include/apex/core/config.hpp apex_core/src/config.cpp \
    apex_core/config/default.toml apex_core/tests/unit/test_config.cpp \
    apex_core/include/apex/core/server.hpp \
    apex_core/CMakeLists.txt apex_core/tests/unit/CMakeLists.txt
git commit -m "feat: AppConfig TOML 설정 시스템 (from_file + defaults)"
```

---

## Task 3: spdlog 로깅 시스템 (TDD)

**Files:**
- Create: `apex_core/include/apex/core/logging.hpp`
- Create: `apex_core/src/logging.cpp`
- Create: `apex_core/tests/unit/test_logging.cpp`
- Modify: `apex_core/CMakeLists.txt` (src/logging.cpp 추가)
- Modify: `apex_core/tests/unit/CMakeLists.txt` (테스트 추가)

### Step 1: logging.hpp 헤더 작성

```cpp
// include/apex/core/logging.hpp
#pragma once

#include <apex/core/config.hpp>

namespace apex::core {

/// LogConfig 기반 spdlog 로거 초기화.
/// "apex" (프레임워크) + "app" (서비스) 로거 생성.
/// main() 초반 1회 호출.
void init_logging(const LogConfig& config);

/// spdlog 정리. Server::run() 리턴 후 호출 (선택적).
void shutdown_logging();

} // namespace apex::core
```

### Step 2: 실패하는 테스트 작성

`apex_core/tests/unit/test_logging.cpp`:
```cpp
#include <apex/core/logging.hpp>

#include <spdlog/spdlog.h>
#include <gtest/gtest.h>

#include <filesystem>

using namespace apex::core;

namespace {

class LoggingTest : public ::testing::Test {
protected:
    void TearDown() override {
        shutdown_logging();
        // 테스트 간 로거 격리
    }
};

} // anonymous namespace

TEST_F(LoggingTest, InitCreatesApexLogger) {
    LogConfig cfg;
    init_logging(cfg);
    auto logger = spdlog::get("apex");
    ASSERT_NE(logger, nullptr);
}

TEST_F(LoggingTest, InitCreatesAppLogger) {
    LogConfig cfg;
    init_logging(cfg);
    auto logger = spdlog::get("app");
    ASSERT_NE(logger, nullptr);
}

TEST_F(LoggingTest, FrameworkLevelIndependent) {
    LogConfig cfg;
    cfg.level = "info";
    cfg.framework_level = "debug";
    init_logging(cfg);

    auto apex = spdlog::get("apex");
    auto app = spdlog::get("app");
    EXPECT_EQ(apex->level(), spdlog::level::debug);
    EXPECT_EQ(app->level(), spdlog::level::info);
}

TEST_F(LoggingTest, ConsoleOnlyByDefault) {
    LogConfig cfg;  // console.enabled=true, file.enabled=false
    init_logging(cfg);
    auto logger = spdlog::get("apex");
    // 기본 설정: ConsoleSink 1개만
    EXPECT_EQ(logger->sinks().size(), 1);
}

TEST_F(LoggingTest, FileEnabled) {
    auto tmp = std::filesystem::temp_directory_path() / "apex_log_test";
    std::filesystem::create_directories(tmp);
    auto log_path = (tmp / "test.log").string();

    LogConfig cfg;
    cfg.file.enabled = true;
    cfg.file.path = log_path;
    init_logging(cfg);

    auto logger = spdlog::get("apex");
    EXPECT_EQ(logger->sinks().size(), 2);  // Console + File

    logger->info("test message");
    logger->flush();

    EXPECT_TRUE(std::filesystem::exists(log_path));
    std::filesystem::remove_all(tmp);
}

TEST_F(LoggingTest, ShutdownCleansUp) {
    LogConfig cfg;
    init_logging(cfg);
    shutdown_logging();
    EXPECT_EQ(spdlog::get("apex"), nullptr);
    EXPECT_EQ(spdlog::get("app"), nullptr);
}
```

### Step 3: 테스트 실행 — 실패 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 링크 에러 (init_logging, shutdown_logging 미구현)

### Step 4: logging.cpp 구현

```cpp
// src/logging.cpp
#include <apex/core/logging.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace apex::core {

namespace {

spdlog::level::level_enum parse_level(const std::string& level_str) {
    // spdlog::level::from_str handles: trace, debug, info, warn, err, critical, off
    return spdlog::level::from_str(level_str);
}

/// JSON 포매터 — FileSink 전용.
/// 출력: {"ts":"...","level":"...","logger":"...","msg":"..."}
class JsonFormatter : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        using namespace std::chrono;
        auto time = msg.time;
        auto ms = duration_cast<milliseconds>(time.time_since_epoch()) % 1000;
        auto tt = system_clock::to_time_t(time);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        // {"ts":"2026-03-08T14:23:01.123","level":"info","logger":"apex","msg":"..."}
        fmt::format_to(std::back_inserter(dest),
            R"({{"ts":"{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}","level":"{}","logger":"{}","msg":")"
            , tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday
            , tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count())
            , spdlog::level::to_string_view(msg.level)
            , msg.logger_name);

        // JSON-escape the message payload
        for (auto c : std::string_view(msg.payload.data(), msg.payload.size())) {
            switch (c) {
                case '"':  dest.push_back('\\'); dest.push_back('"'); break;
                case '\\': dest.push_back('\\'); dest.push_back('\\'); break;
                case '\n': dest.push_back('\\'); dest.push_back('n'); break;
                case '\r': dest.push_back('\\'); dest.push_back('r'); break;
                case '\t': dest.push_back('\\'); dest.push_back('t'); break;
                default:   dest.push_back(c); break;
            }
        }
        dest.push_back('"');
        dest.push_back('}');
        dest.push_back('\n');
    }

    [[nodiscard]] std::unique_ptr<formatter> clone() const override {
        return std::make_unique<JsonFormatter>();
    }
};

} // anonymous namespace

void init_logging(const LogConfig& config) {
    // 기존 로거 정리 (재초기화 지원)
    spdlog::drop_all();

    std::vector<spdlog::sink_ptr> sinks;

    // ConsoleSink — 텍스트 포맷
    if (config.console.enabled) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(config.pattern);
        sinks.push_back(console_sink);
    }

    // FileSink — JSON 포맷 (rotating)
    if (config.file.enabled) {
        auto dir = std::filesystem::path(config.file.path).parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
        }
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.file.path,
            config.file.max_size_mb * 1024 * 1024,
            config.file.max_files);
        if (config.file.json) {
            file_sink->set_formatter(std::make_unique<JsonFormatter>());
        }
        sinks.push_back(file_sink);
    }

    // "apex" 로거 (프레임워크)
    auto apex_logger = std::make_shared<spdlog::logger>("apex", sinks.begin(), sinks.end());
    apex_logger->set_level(parse_level(config.framework_level));
    spdlog::register_logger(apex_logger);

    // "app" 로거 (서비스)
    auto app_logger = std::make_shared<spdlog::logger>("app", sinks.begin(), sinks.end());
    app_logger->set_level(parse_level(config.level));
    spdlog::register_logger(app_logger);
}

void shutdown_logging() {
    spdlog::shutdown();
}

} // namespace apex::core
```

### Step 5: CMakeLists.txt에 소스 + 테스트 등록

`apex_core/CMakeLists.txt` — `add_library(apex_core STATIC ...)` 목록에 `src/logging.cpp` 추가.

`apex_core/tests/unit/CMakeLists.txt` — 끝에 추가:
```cmake
apex_add_unit_test(test_logging test_logging.cpp)
```

### Step 6: 빌드 + 테스트 통과 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 전체 테스트 통과 (기존 + test_config + test_logging)

### Step 7: Commit

```bash
git add apex_core/include/apex/core/logging.hpp apex_core/src/logging.cpp \
    apex_core/tests/unit/test_logging.cpp \
    apex_core/CMakeLists.txt apex_core/tests/unit/CMakeLists.txt
git commit -m "feat: spdlog 로깅 시스템 (apex/app 2레벨, Console텍스트 + FileJSON)"
```

---

## Task 4: Graceful Shutdown 타임아웃 (TDD)

**Files:**
- Create: `apex_core/tests/integration/test_shutdown_timeout.cpp`
- Modify: `apex_core/include/apex/core/server.hpp` (shutdown_deadline_ 멤버)
- Modify: `apex_core/src/server.cpp` (poll_shutdown 타임아웃 로직)
- Modify: `apex_core/tests/integration/CMakeLists.txt` (테스트 추가)

### Step 1: 실패하는 통합 테스트 작성

`apex_core/tests/integration/test_shutdown_timeout.cpp`:
```cpp
#include <apex/core/server.hpp>
#include <apex/core/logging.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace apex::core;
using namespace std::chrono_literals;

namespace {

void wait_until_running(Server& server, std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!server.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

} // anonymous namespace

TEST(ShutdownTimeoutTest, NormalShutdownWithinTimeout) {
    // 세션 없이 바로 종료 — 타임아웃 전에 완료되어야 함
    init_logging(LogConfig{});

    Server server({
        .port = 0,
        .num_cores = 1,
        .handle_signals = false,
        .drain_timeout = 2s,
    });

    std::thread t([&] { server.run(); });
    wait_until_running(server);

    auto start = std::chrono::steady_clock::now();
    server.stop();
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 세션 없으면 즉시 종료 (2초 타임아웃보다 훨씬 빨라야 함)
    EXPECT_LT(elapsed, 1s);

    shutdown_logging();
}

TEST(ShutdownTimeoutTest, DrainTimeoutForcesShutdown) {
    // drain_timeout이 짧으면 강제 종료까지의 시간이 제한됨
    init_logging(LogConfig{});

    Server server({
        .port = 0,
        .num_cores = 1,
        .handle_signals = false,
        .drain_timeout = 1s,  // 1초 타임아웃
    });

    std::thread t([&] { server.run(); });
    wait_until_running(server);
    server.stop();
    t.join();

    // 서버가 정상 종료됨 (크래시/행 없이)
    EXPECT_FALSE(server.running());

    shutdown_logging();
}
```

### Step 2: 테스트 실행 — 실패 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 빌드는 되지만, shutdown_deadline_ 관련 로직이 없어서 동작이 달라질 수 있음.
(이 테스트는 타임아웃 강제 종료 로직이 없어도 통과할 수 있으므로, 실제 구현과 함께 검증)

### Step 3: server.hpp 수정

`server.hpp` — private 멤버 추가:
```cpp
std::chrono::steady_clock::time_point shutdown_deadline_;
```

### Step 4: server.cpp 수정

`server.cpp` — `#include <apex/core/logging.hpp>` 추가 (spdlog 사용).

`begin_shutdown()` 수정:
```cpp
void Server::begin_shutdown() {
    shutdown_deadline_ = std::chrono::steady_clock::now() + config_.drain_timeout;

    for (uint32_t i = 0; i < config_.num_cores; ++i) {
        boost::asio::post(core_engine_->io_context(i),
            [this, i] {
                per_core_[i]->session_mgr.for_each([](SessionPtr session) {
                    session->close();
                });
            });
    }

    shutdown_timer_ = std::make_unique<boost::asio::steady_timer>(accept_io_);
    poll_shutdown();
}
```

`poll_shutdown()` 수정:
```cpp
void Server::poll_shutdown() {
    if (active_sessions_.load(std::memory_order_acquire) == 0) {
        shutdown_timer_.reset();
        spdlog::get("apex")->info("All sessions drained, shutting down");

        core_engine_->stop();
        core_engine_->join();
        core_engine_->drain_remaining();

        for (auto& state : per_core_) {
            for (auto& svc : state->services) {
                svc->stop();
            }
        }

        accept_io_.stop();
        return;
    }

    // 타임아웃 체크
    if (std::chrono::steady_clock::now() >= shutdown_deadline_) {
        shutdown_timer_.reset();
        spdlog::get("apex")->warn(
            "Drain timeout ({}s) expired, {} sessions remaining — forcing shutdown",
            config_.drain_timeout.count(),
            active_sessions_.load(std::memory_order_acquire));

        core_engine_->stop();
        core_engine_->join();
        core_engine_->drain_remaining();

        for (auto& state : per_core_) {
            for (auto& svc : state->services) {
                svc->stop();
            }
        }

        accept_io_.stop();
        return;
    }

    shutdown_timer_->expires_after(std::chrono::milliseconds(1));
    shutdown_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) return;
        poll_shutdown();
    });
}
```

**참고**: 정상/강제 종료의 시퀀스가 동일하므로 private 헬퍼(`finalize_shutdown()`)로 추출 가능. 구현자 판단에 맡김.

### Step 5: 기존 테스트에 init_logging 추가

기존 서버 테스트(test_server_multicore, test_server_e2e 등)에서 spdlog 로거가 없으면 `spdlog::get("apex")` 반환값이 nullptr이라 크래시 가능.
→ 테스트 fixture 또는 개별 테스트에 `init_logging(LogConfig{})` + `shutdown_logging()` 추가.
→ 또는 `server.cpp`에서 `spdlog::get("apex")`가 nullptr이면 로그 스킵하는 null check 추가.

**추천**: null check 방식. 프레임워크가 로거 없이도 동작해야 기존 테스트 호환성 유지.
```cpp
if (auto logger = spdlog::get("apex")) {
    logger->info("...");
}
```

### Step 6: 빌드 + 테스트 통과 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 전체 테스트 통과

### Step 7: Commit

```bash
git add apex_core/include/apex/core/server.hpp apex_core/src/server.cpp \
    apex_core/tests/integration/test_shutdown_timeout.cpp \
    apex_core/tests/integration/CMakeLists.txt
git commit -m "feat: Graceful Shutdown drain 타임아웃 (기본 25초, TOML 설정)"
```

---

## Task 5: 프레임워크 std::cout → spdlog 전환

**Files:**
- Modify: `apex_core/src/server.cpp` (std::cout 제거)
- Modify: `apex_core/src/core_engine.cpp` (std::cout 있으면 제거)
- Modify: `apex_core/src/tcp_acceptor.cpp` (std::cout 있으면 제거)
- Modify: `apex_core/examples/echo_server.cpp`
- Modify: `apex_core/examples/multicore_echo_server.cpp`
- Modify: `apex_core/examples/chat_server.cpp`

### Step 1: 프레임워크 소스에서 std::cout 검색

Run: `grep -rn "std::cout\|std::cerr\|std::clog" apex_core/src/`
→ 발견된 모든 곳을 `spdlog::get("apex")->info/warn/error(...)` 또는 null-safe 래퍼로 대체.

### Step 2: 예제 코드 업데이트

예제에 TOML 로딩을 필수로 넣지 않음 — 기존처럼 간결하게 유지하되, `init_logging` 호출만 추가.

`echo_server.cpp` 변경 예시:
```cpp
#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
// ...

int main(int argc, char* argv[]) {
    auto config = AppConfig::defaults();
    // CLI 인자로 port, cores 오버라이드 (기존 로직 유지)
    // ...
    config.server.port = port;
    config.server.num_cores = cores;

    init_logging(config.logging);

    spdlog::get("app")->info("=== Apex Pipeline Echo Server v0.2.4 ===");
    spdlog::get("app")->info("Port: {}, Cores: {}", port, cores);

    Server(config.server)
        .add_service<EchoService>()
        .run();

    shutdown_logging();
    return 0;
}
```

### Step 3: 빌드 + 테스트 통과 확인

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 전체 테스트 통과, std::cout/cerr 사용 제거됨

### Step 4: Commit

```bash
git add apex_core/src/ apex_core/examples/
git commit -m "refactor: std::cout → spdlog 전환 (프레임워크 + 예제)"
```

---

## Task 6: Linux 빌드 호환성

**Files:**
- Modify: `CMakePresets.json` (Linux 전용 프리셋 추가)
- Modify: `apex_core/build.sh` (동작 검증/수정)
- Create: `apex_core/tsan_suppressions.txt`
- Modify: `apex_core/src/*.cpp`, `include/**/*.hpp` (플랫폼 분기 수정, 필요 시)

### Step 1: CMakePresets.json에 Linux 전용 프리셋 추가

ASAN/TSAN 프리셋은 이미 존재(`asan`, `tsan`). 추가로 linker 플래그가 필요한지 확인:

`CMakePresets.json` — asan, tsan 프리셋에 `CMAKE_EXE_LINKER_FLAGS` 추가:
```json
{
    "name": "asan",
    "inherits": "debug",
    "cacheVariables": {
        "CMAKE_CXX_FLAGS": "-fsanitize=address -fno-omit-frame-pointer",
        "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=address",
        "APEX_BUILD_VARIANT": "asan"
    }
},
{
    "name": "tsan",
    "inherits": "debug",
    "cacheVariables": {
        "CMAKE_CXX_FLAGS": "-fsanitize=thread",
        "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=thread",
        "APEX_BUILD_VARIANT": "tsan"
    }
}
```

### Step 2: TSAN suppressions 파일 생성

`apex_core/tsan_suppressions.txt`:
```
# Boost.Asio 내부 false positive 대비
# 실제 발생 시 여기에 패턴 추가
# race:boost::asio::detail::*
```

CMakePresets.json의 tsan 프리셋에 환경변수 추가:
```json
"environment": {
    "TSAN_OPTIONS": "suppressions=${sourceDir}/apex_core/tsan_suppressions.txt"
}
```

### Step 3: 플랫폼 분기 코드 검토

소스에서 `_WIN32`, `WIN32`, `_MSC_VER`, `gmtime_s` 등 Windows 전용 코드를 검색하여 Linux 분기 추가.
→ `logging.cpp`의 `gmtime_s`/`gmtime_r`은 이미 분기됨 (Step 4에서 작성).
→ `server.cpp`의 `SIGINT`/`SIGTERM`은 Boost.Asio signal_set이 크로스 플랫폼 처리.
→ `_aligned_malloc`/`_aligned_free`가 있으면 `std::aligned_alloc`/`std::free` 분기 확인.

### Step 4: 로컬에서는 Windows 빌드만 검증

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"`
Expected: 전체 테스트 통과
(Linux 빌드 검증은 CI에서 수행 — Task 7)

### Step 5: Commit

```bash
git add CMakePresets.json apex_core/tsan_suppressions.txt apex_core/build.sh
# 플랫폼 분기 수정한 소스도 포함
git commit -m "build: Linux 빌드 호환성 (ASAN/TSAN 링커 플래그, suppressions)"
```

---

## Task 7: CI/CD GitHub Actions

**Files:**
- Create: `.github/workflows/ci.yml`

### Step 1: 워크플로우 작성

`.github/workflows/ci.yml`:
```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: windows-msvc
            os: windows-latest
            preset: debug
          - name: linux-gcc
            os: ubuntu-latest
            preset: debug
          - name: linux-asan
            os: ubuntu-latest
            preset: asan
          - name: linux-tsan
            os: ubuntu-latest
            preset: tsan

    name: ${{ matrix.name }}
    runs-on: ${{ matrix.os }}

    env:
      VCPKG_ROOT: ${{ github.workspace }}/vcpkg

    steps:
      - uses: actions/checkout@v4

      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: 'b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01'

      - name: Setup Ninja
        uses: seanmiddleditch/gha-setup-ninja@master

      - name: Configure
        working-directory: apex_core
        run: cmake --preset ${{ matrix.preset }}

      - name: Build
        working-directory: apex_core
        run: cmake --build build/${{ matrix.preset }}

      - name: Test
        working-directory: apex_core
        run: ctest --test-dir build/${{ matrix.preset }} --output-on-failure
```

**참고**:
- `lukka/run-vcpkg`가 자동으로 vcpkg 캐싱을 처리함 (actions/cache 내장).
- `vcpkgGitCommitId`는 `vcpkg.json`의 `builtin-baseline`과 일치시킴.
- `fail-fast: false` — 한 잡이 실패해도 나머지 잡은 계속 실행 (전체 매트릭스 결과 확인 가능).
- Windows에서는 vcvarsall.bat 설정이 필요할 수 있음 — `lukka/run-cmake` 또는 `ilammy/msvc-dev-cmd` 액션으로 해결.

### Step 2: Commit

```bash
git add .github/workflows/ci.yml
git commit -m "ci: GitHub Actions 4잡 매트릭스 (MSVC, GCC, ASAN, TSAN)"
```

---

## 병렬 실행 가이드

```
Task 1 (의존성) ──→ Task 2 (Config) ──→ Task 3 (Logging) ──→ Task 4 (Shutdown) ──→ Task 5 (Migration)
                ╲                                                                   ╱
                 └─→ Task 6 (Linux 호환) ──→ Task 7 (CI/CD) ──────────────────────╱
```

- Task 1 완료 후: Task 2 + Task 6 **병렬 가능**
- Task 3 완료 후: Task 4 + Task 5 **병렬 가능** (파일 비겹침 조건: Task 5는 examples만, Task 4는 server.cpp만)
- Task 7은 Task 6 완료 후 진행 (Linux 프리셋이 있어야 CI 구성 가능)

## 파일 충돌 방지 매트릭스

| 파일 | Task 1 | Task 2 | Task 3 | Task 4 | Task 5 | Task 6 | Task 7 |
|------|--------|--------|--------|--------|--------|--------|--------|
| CMakeLists.txt | W | W | W | - | - | - | - |
| server.hpp | - | W | - | W | - | - | - |
| server.cpp | - | - | - | W | W | - | - |
| vcpkg.json | W | - | - | - | - | - | - |
| CMakePresets.json | - | - | - | - | - | W | - |

**주의**: server.cpp는 Task 4(타임아웃 로직)와 Task 5(std::cout 제거)가 모두 수정 → **순차 필수**.
