// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/server_config.hpp>
#include <string>

namespace apex::core
{

struct LogConsoleConfig
{
    bool enabled = true;
};

struct LogFileConfig
{
    bool enabled = false;
    std::string path; // 빈 값 → {project_root}/logs/. 설정 시 그대로 사용
    bool json = false;
};

struct LogAsyncConfig
{
    size_t queue_size = 8192;
};

struct LogConfig
{
    std::string level = "info";
    std::string framework_level = "info";
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v";
    std::string service_name; // 빈 값 시 "default" 사용
    LogConsoleConfig console;
    LogFileConfig file;
    LogAsyncConfig async;
};

struct AppConfig
{
    ServerConfig server;
    LogConfig logging;

    /// TOML 파일에서 설정 로딩. 파일 없으면 std::runtime_error.
    static AppConfig from_file(const std::string& path);

    /// 기본값으로 생성 (TOML 파일 불필요).
    static AppConfig defaults();
};

} // namespace apex::core
