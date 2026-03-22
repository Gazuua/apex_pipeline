// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/redis/redis_config.hpp>
#include <apex/shared/adapters/redis/redis_multiplexer.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace apex::shared::adapters::redis
{

/// Redis adapter. Registered as a single global instance per Server.
/// Internally manages per-core RedisMultiplexer instances.
///
/// Usage:
///   Server({.port = 9000, .num_cores = 4})
///       .add_adapter<RedisAdapter>(redis_config)
///       .add_service<MyService>()
///       .run();
///
///   // Inside a service handler:
///   auto& redis = server.adapter<RedisAdapter>();
///   auto& mux = redis.multiplexer(core_id);
///   auto val = co_await mux.command("GET %s", "key");
class RedisAdapter : public AdapterBase<RedisAdapter>
{
  public:
    explicit RedisAdapter(RedisConfig config);
    ~RedisAdapter();

    // --- AdapterBase CRTP implementation ---

    /// Create per-core RedisMultiplexer instances.
    void do_init(apex::core::CoreEngine& engine);

    /// Signal to reject new requests.
    void do_drain();

    /// Cleanup all multiplexers.
    void do_close();

    /// Per-core multiplexer close (called by AdapterBase::close Phase 1).
    void do_close_per_core(uint32_t core_id);

    /// Adapter name.
    [[nodiscard]] std::string_view do_name() const noexcept
    {
        return "redis";
    }

    // --- Multiplexer access ---

    /// Return the multiplexer for the given core.
    [[nodiscard]] RedisMultiplexer& multiplexer(uint32_t core_id);

    // --- Monitoring API ---

    /// Count of multiplexers with active connections.
    [[nodiscard]] std::size_t active_connections() const noexcept;

    /// Config access.
    [[nodiscard]] const RedisConfig& config() const noexcept
    {
        return config_;
    }

  private:
    RedisConfig config_;
    std::vector<std::unique_ptr<RedisMultiplexer>> per_core_;
};

} // namespace apex::shared::adapters::redis
