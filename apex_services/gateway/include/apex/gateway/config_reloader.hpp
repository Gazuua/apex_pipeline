#pragma once

#include <apex/gateway/file_watcher.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>
#include <apex/gateway/route_table.hpp>
#include <apex/shared/rate_limit/endpoint_rate_config.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apex::gateway
{

/// Convert TOML RateLimitEndpointConfig -> shared EndpointRateConfig.
/// Used during hot-reload to feed RateLimitFacade::update_endpoint_config().
[[nodiscard]] apex::shared::rate_limit::EndpointRateConfig to_endpoint_rate_config(const RateLimitEndpointConfig& src);

/// TOML file change detection -> RouteTable + RateLimit config atomic replacement.
/// When FileWatcher detects change:
/// 1. Re-parse TOML
/// 2. Build new RouteTable + validate
/// 3. Distribute new table to all per-core MessageRouters
/// 4. Distribute new RateLimit config to all per-core rate limiters
///
/// Thread safety: RouteUpdate and RateLimitUpdate callbacks are posted to each
/// per-core io_context, ensuring updates execute on the owning core thread
/// (shared-nothing). No cross-thread mutation of per-core state.
class ConfigReloader
{
  public:
    using RouteUpdateCallback = std::function<void(RouteTablePtr)>;
    using RateLimitUpdateCallback = std::function<void(const RateLimitConfig&)>;

    /// @param io_ctx io_context for the FileWatcher timer.
    /// @param config_path TOML config file path.
    /// @param core_io_contexts Per-core io_context references for post-based dispatch.
    ///   If empty, callbacks fire directly on the watcher io_ctx (legacy behavior).
    /// @param on_route_update Per-core route table update callback.
    /// @param on_rate_limit_update Per-core rate limit config update callback (optional).
    ConfigReloader(boost::asio::io_context& io_ctx, std::string config_path,
                   std::vector<std::reference_wrapper<boost::asio::io_context>> core_io_contexts,
                   RouteUpdateCallback on_route_update, RateLimitUpdateCallback on_rate_limit_update = nullptr);

    /// Legacy constructor (no per-core post — callbacks fire on watcher thread).
    ConfigReloader(boost::asio::io_context& io_ctx, std::string config_path, RouteUpdateCallback on_route_update,
                   RateLimitUpdateCallback on_rate_limit_update = nullptr);

    /// Start watching.
    void start();

    /// Stop watching.
    void stop();

  private:
    void on_file_changed(const std::string& path);

    std::string config_path_;
    std::vector<std::reference_wrapper<boost::asio::io_context>> core_io_contexts_;
    RouteUpdateCallback on_route_update_;
    RateLimitUpdateCallback on_rate_limit_update_;
    std::unique_ptr<FileWatcher> watcher_;
};

} // namespace apex::gateway
