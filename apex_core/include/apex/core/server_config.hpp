// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace apex::core
{

/// Prometheus metrics endpoint configuration.
struct MetricsConfig
{
    bool enabled = false;
    uint16_t port = 8081;
};

/// Admin HTTP server configuration (runtime management).
struct AdminConfig
{
    bool enabled = false;
    uint16_t port = 8082;
};

/// Server configuration. Fields are ordered for designated-initializer convenience.
struct ServerConfig
{
    // Network
    // port 제거 — listen<P>(port, config)으로 대체
    std::string bind_address = "0.0.0.0"; // Bind address (e.g., "127.0.0.1" for loopback only)
    bool tcp_nodelay = true;              // Disable Nagle's algorithm for low-latency

    // Multicore
    uint32_t num_cores = 1;
    size_t spsc_queue_capacity = 1024;
    std::chrono::milliseconds tick_interval{100};

    // Session
    uint32_t heartbeat_timeout_ticks = 300; // 0 = disabled
    size_t recv_buf_capacity = 8192;
    size_t session_max_queue_depth = 256; // Per-session write queue depth limit
    size_t timer_wheel_slots = 1024;

    // Connection limits
    uint32_t max_connections = 10000; // Maximum concurrent connections (0 = unlimited, default 10000)

    // Platform I/O
    bool reuseport = false; // Linux: per-core SO_REUSEPORT, Windows: ignored

    // Lifecycle
    bool handle_signals = true;
    std::chrono::seconds drain_timeout{25}; // Graceful Shutdown drain timeout

    // Cross-core call
    std::chrono::milliseconds cross_core_call_timeout{5000};

    // Memory allocators (per-core)
    std::size_t bump_capacity_bytes = 64 * 1024; // 64KB
    std::size_t arena_block_bytes = 4096;        // 4KB
    std::size_t arena_max_bytes = 1024 * 1024;   // 1MB

    // Metrics
    MetricsConfig metrics;

    // Admin (runtime management — log level, etc.)
    AdminConfig admin;
};

} // namespace apex::core
