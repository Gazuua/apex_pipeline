// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/config.hpp>
#include <apex/core/scoped_logger.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace apex::core
{

namespace
{

const ScopedLogger& s_logger()
{
    static const ScopedLogger instance{"Config", ScopedLogger::NO_CORE};
    return instance;
}

template <typename T> T get_or(const toml::table& tbl, std::string_view key, const T& default_val)
{
    auto node = tbl[key];
    if (!node)
    {
        s_logger().debug("key '{}' not set, using default", key);
        return default_val;
    }
    auto val = node.value<T>();
    if (!val)
    {
        throw std::invalid_argument(std::string("Config: invalid type for key '") + std::string(key) + "'");
    }
    return *val;
}

template <typename Target> Target checked_narrow(int64_t value, std::string_view key)
{
    if constexpr (std::is_unsigned_v<Target> && sizeof(Target) >= sizeof(int64_t))
    {
        // size_t(uint64_t) — int64_t 음수만 검증 (양수는 항상 범위 내)
        if (value < 0)
        {
            throw std::invalid_argument(std::string("Config: value out of range for '") + std::string(key) + "'");
        }
    }
    else
    {
        if (value < static_cast<int64_t>(std::numeric_limits<Target>::min()) ||
            value > static_cast<int64_t>(std::numeric_limits<Target>::max()))
        {
            throw std::invalid_argument(std::string("Config: value out of range for '") + std::string(key) + "'");
        }
    }
    return static_cast<Target>(value);
}

ServerConfig parse_server(const toml::table& root)
{
    ServerConfig cfg;
    auto* tbl = root["server"].as_table();
    if (!tbl)
        return cfg;

    // port 제거 — listen<P>(port, config) API로 대체 (v0.5)
    // TOML의 port 필드는 무시 (하위 호환)
    cfg.num_cores = checked_narrow<uint32_t>(get_or<int64_t>(*tbl, "num_cores", cfg.num_cores), "num_cores");
    // I-20: Use int64_t default to avoid narrowing conversion warning
    cfg.spsc_queue_capacity =
        checked_narrow<size_t>(get_or<int64_t>(*tbl, "spsc_queue_capacity", int64_t{1024}), "spsc_queue_capacity");
    {
        auto tick_ms = get_or<int64_t>(*tbl, "tick_interval_ms", cfg.tick_interval.count());
        if (tick_ms < 0)
        {
            throw std::invalid_argument("Config: value out of range for 'tick_interval_ms'");
        }
        cfg.tick_interval = std::chrono::milliseconds(tick_ms);
    }
    cfg.heartbeat_timeout_ticks = checked_narrow<uint32_t>(
        get_or<int64_t>(*tbl, "heartbeat_timeout_ticks", cfg.heartbeat_timeout_ticks), "heartbeat_timeout_ticks");
    // I-20: Use int64_t literals for size_t defaults to avoid narrowing warnings
    cfg.recv_buf_capacity =
        checked_narrow<size_t>(get_or<int64_t>(*tbl, "recv_buf_capacity", int64_t{8192}), "recv_buf_capacity");
    cfg.timer_wheel_slots =
        checked_narrow<size_t>(get_or<int64_t>(*tbl, "timer_wheel_slots", int64_t{1024}), "timer_wheel_slots");
    cfg.handle_signals = get_or<bool>(*tbl, "handle_signals", cfg.handle_signals);
    {
        auto drain_s = get_or<int64_t>(*tbl, "drain_timeout_s", cfg.drain_timeout.count());
        if (drain_s < 0)
        {
            throw std::invalid_argument("Config: value out of range for 'drain_timeout_s'");
        }
        cfg.drain_timeout = std::chrono::seconds(drain_s);
    }

    // cross_core_call_timeout
    {
        auto timeout_ms = get_or<int64_t>(*tbl, "cross_core_call_timeout_ms", cfg.cross_core_call_timeout.count());
        if (timeout_ms < 0)
        {
            throw std::invalid_argument("Config: value out of range for 'cross_core_call_timeout_ms'");
        }
        cfg.cross_core_call_timeout = std::chrono::milliseconds(timeout_ms);
    }

    // [server.memory] subsection
    if (auto* mem = (*tbl)["memory"].as_table())
    {
        if (auto v = (*mem)["bump_capacity_kb"].value<int64_t>())
        {
            if (*v < 0)
            {
                throw std::invalid_argument("Config: value out of range for 'bump_capacity_kb'");
            }
            cfg.bump_capacity_bytes = static_cast<size_t>(*v) * 1024;
        }
        if (auto v = (*mem)["arena_block_kb"].value<int64_t>())
        {
            if (*v < 0)
            {
                throw std::invalid_argument("Config: value out of range for 'arena_block_kb'");
            }
            cfg.arena_block_bytes = static_cast<size_t>(*v) * 1024;
        }
        if (auto v = (*mem)["arena_max_kb"].value<int64_t>())
        {
            if (*v < 0)
            {
                throw std::invalid_argument("Config: value out of range for 'arena_max_kb'");
            }
            cfg.arena_max_bytes = static_cast<size_t>(*v) * 1024;
        }
    }

    return cfg;
}

LogConfig parse_logging(const toml::table& root)
{
    LogConfig cfg;
    auto* tbl = root["logging"].as_table();
    if (!tbl)
        return cfg;

    cfg.level = get_or<std::string>(*tbl, "level", cfg.level);
    cfg.framework_level = get_or<std::string>(*tbl, "framework_level", cfg.framework_level);
    cfg.pattern = get_or<std::string>(*tbl, "pattern", cfg.pattern);
    cfg.service_name = get_or<std::string>(*tbl, "service_name", cfg.service_name);

    // [logging.console]
    if (auto* con = (*tbl)["console"].as_table())
    {
        cfg.console.enabled = get_or<bool>(*con, "enabled", cfg.console.enabled);
    }

    // [logging.file]
    if (auto* file = (*tbl)["file"].as_table())
    {
        cfg.file.enabled = get_or<bool>(*file, "enabled", cfg.file.enabled);
        cfg.file.path = get_or<std::string>(*file, "path", cfg.file.path);
        cfg.file.json = get_or<bool>(*file, "json", cfg.file.json);
        // retention 관련 필드 없음 — 로그 영구 보존 의도적 설계 (WONTFIX)
    }

    // [logging.async]
    if (auto* async_tbl = (*tbl)["async"].as_table())
    {
        cfg.async.queue_size =
            checked_narrow<size_t>(get_or<int64_t>(*async_tbl, "queue_size", int64_t{8192}), "queue_size");
    }

    return cfg;
}

MetricsConfig parse_metrics(const toml::table& root)
{
    MetricsConfig cfg;
    auto* tbl = root["metrics"].as_table();
    if (!tbl)
        return cfg;

    cfg.enabled = get_or<bool>(*tbl, "enabled", cfg.enabled);
    cfg.port = checked_narrow<uint16_t>(get_or<int64_t>(*tbl, "port", int64_t{8081}), "metrics.port");
    return cfg;
}

AdminConfig parse_admin(const toml::table& root)
{
    AdminConfig cfg;
    auto* tbl = root["admin"].as_table();
    if (!tbl)
        return cfg;

    cfg.enabled = get_or<bool>(*tbl, "enabled", cfg.enabled);
    cfg.port = checked_narrow<uint16_t>(get_or<int64_t>(*tbl, "port", int64_t{8082}), "admin.port");
    return cfg;
}

} // anonymous namespace

AppConfig AppConfig::from_file(const std::string& path)
{
    s_logger().info("loading config from '{}'", path);
    toml::table tbl;
    try
    {
        tbl = toml::parse_file(path);
    }
    catch (const toml::parse_error& e)
    {
        throw std::runtime_error(std::string("Failed to parse TOML config '") + path +
                                 "': " + std::string(e.description()));
    }

    AppConfig config;
    config.server = parse_server(tbl);
    config.logging = parse_logging(tbl);
    config.metrics = parse_metrics(tbl);
    config.admin = parse_admin(tbl);
    // Sync metrics/admin config into ServerConfig so Server can access it directly
    config.server.metrics = config.metrics;
    config.server.admin = config.admin;
    s_logger().info("config loaded: cores={}, log_level={}", config.server.num_cores, config.logging.level);
    return config;
}

AppConfig AppConfig::defaults()
{
    return AppConfig{};
}

} // namespace apex::core
