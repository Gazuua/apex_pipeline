#include <apex/core/config.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

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

template <typename Target>
Target checked_narrow(int64_t value, std::string_view key) {
    if constexpr (std::is_unsigned_v<Target> && sizeof(Target) >= sizeof(int64_t)) {
        // size_t(uint64_t) — int64_t 음수만 검증 (양수는 항상 범위 내)
        if (value < 0) {
            throw std::invalid_argument(
                std::string("Config: value out of range for '") + std::string(key) + "'");
        }
    } else {
        if (value < static_cast<int64_t>(std::numeric_limits<Target>::min()) ||
            value > static_cast<int64_t>(std::numeric_limits<Target>::max())) {
            throw std::invalid_argument(
                std::string("Config: value out of range for '") + std::string(key) + "'");
        }
    }
    return static_cast<Target>(value);
}

ServerConfig parse_server(const toml::table& root) {
    ServerConfig cfg;
    auto* tbl = root["server"].as_table();
    if (!tbl) return cfg;

    // port 제거 — listen<P>(port, config) API로 대체 (v0.5)
    // TOML의 port 필드는 무시 (하위 호환)
    cfg.num_cores = checked_narrow<uint32_t>(
        get_or<int64_t>(*tbl, "num_cores", cfg.num_cores), "num_cores");
    // I-20: Use int64_t default to avoid narrowing conversion warning
    cfg.mpsc_queue_capacity = checked_narrow<size_t>(
        get_or<int64_t>(*tbl, "mpsc_queue_capacity", int64_t{65536}),
        "mpsc_queue_capacity");
    {
        auto tick_ms = get_or<int64_t>(*tbl, "tick_interval_ms", cfg.tick_interval.count());
        if (tick_ms < 0) {
            throw std::invalid_argument("Config: value out of range for 'tick_interval_ms'");
        }
        cfg.tick_interval = std::chrono::milliseconds(tick_ms);
    }
    cfg.heartbeat_timeout_ticks = checked_narrow<uint32_t>(
        get_or<int64_t>(*tbl, "heartbeat_timeout_ticks", cfg.heartbeat_timeout_ticks),
        "heartbeat_timeout_ticks");
    // I-20: Use int64_t literals for size_t defaults to avoid narrowing warnings
    cfg.recv_buf_capacity = checked_narrow<size_t>(
        get_or<int64_t>(*tbl, "recv_buf_capacity", int64_t{8192}),
        "recv_buf_capacity");
    cfg.timer_wheel_slots = checked_narrow<size_t>(
        get_or<int64_t>(*tbl, "timer_wheel_slots", int64_t{1024}),
        "timer_wheel_slots");
    cfg.handle_signals = get_or<bool>(*tbl, "handle_signals", cfg.handle_signals);
    {
        auto drain_s = get_or<int64_t>(*tbl, "drain_timeout_s", cfg.drain_timeout.count());
        if (drain_s < 0) {
            throw std::invalid_argument("Config: value out of range for 'drain_timeout_s'");
        }
        cfg.drain_timeout = std::chrono::seconds(drain_s);
    }

    // cross_core_call_timeout
    {
        auto timeout_ms = get_or<int64_t>(*tbl, "cross_core_call_timeout_ms",
                                           cfg.cross_core_call_timeout.count());
        if (timeout_ms < 0) {
            throw std::invalid_argument("Config: value out of range for 'cross_core_call_timeout_ms'");
        }
        cfg.cross_core_call_timeout = std::chrono::milliseconds(timeout_ms);
    }

    // [server.memory] subsection
    if (auto* mem = (*tbl)["memory"].as_table()) {
        if (auto v = (*mem)["bump_capacity_kb"].value<int64_t>()) {
            if (*v < 0) {
                throw std::invalid_argument("Config: value out of range for 'bump_capacity_kb'");
            }
            cfg.bump_capacity_bytes = static_cast<size_t>(*v) * 1024;
        }
        if (auto v = (*mem)["arena_block_kb"].value<int64_t>()) {
            if (*v < 0) {
                throw std::invalid_argument("Config: value out of range for 'arena_block_kb'");
            }
            cfg.arena_block_bytes = static_cast<size_t>(*v) * 1024;
        }
        if (auto v = (*mem)["arena_max_kb"].value<int64_t>()) {
            if (*v < 0) {
                throw std::invalid_argument("Config: value out of range for 'arena_max_kb'");
            }
            cfg.arena_max_bytes = static_cast<size_t>(*v) * 1024;
        }
    }

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
        cfg.file.max_size_mb = checked_narrow<size_t>(
            get_or<int64_t>(*file, "max_size_mb", int64_t{100}),
            "max_size_mb");
        cfg.file.max_files = checked_narrow<size_t>(
            get_or<int64_t>(*file, "max_files", int64_t{3}),
            "max_files");
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
