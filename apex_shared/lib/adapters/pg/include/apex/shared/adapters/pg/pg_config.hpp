// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace apex::shared::adapters::pg
{

/// PostgreSQL adapter configuration.
/// Default connection string points to PgBouncer (port 6432).
struct PgAdapterConfig
{
    std::string connection_string = "host=localhost port=6432 dbname=apex user=apex";
    size_t pool_size_per_core = 2;
    std::chrono::seconds max_idle_time{120};
    std::chrono::seconds health_check_interval{30};

    // acquire_with_retry settings
    uint32_t max_acquire_retries = 3;
    std::chrono::milliseconds retry_backoff{100}; ///< initial backoff (exponential)
};

} // namespace apex::shared::adapters::pg
