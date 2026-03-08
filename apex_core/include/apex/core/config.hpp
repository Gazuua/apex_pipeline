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
