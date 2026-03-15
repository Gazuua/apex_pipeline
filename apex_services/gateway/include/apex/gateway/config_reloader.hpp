#pragma once

#include <apex/gateway/file_watcher.hpp>
#include <apex/gateway/route_table.hpp>
#include <apex/gateway/gateway_config.hpp>
#include <apex/gateway/gateway_config_parser.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apex::gateway {

/// TOML file change detection -> RouteTable + RateLimit config atomic replacement.
/// When FileWatcher detects change:
/// 1. Re-parse TOML
/// 2. Build new RouteTable + validate
/// 3. Distribute new table to all per-core MessageRouters
/// 4. Distribute new RateLimit config to all per-core rate limiters
class ConfigReloader {
public:
    using RouteUpdateCallback = std::function<void(RouteTablePtr)>;
    using RateLimitUpdateCallback = std::function<void(const RateLimitConfig&)>;

    ConfigReloader(boost::asio::io_context& io_ctx,
                   std::string config_path,
                   RouteUpdateCallback on_route_update,
                   RateLimitUpdateCallback on_rate_limit_update = nullptr);

    /// Start watching.
    void start();

    /// Stop watching.
    void stop();

private:
    void on_file_changed(const std::string& path);

    std::string config_path_;
    RouteUpdateCallback on_route_update_;
    RateLimitUpdateCallback on_rate_limit_update_;
    std::unique_ptr<FileWatcher> watcher_;
};

} // namespace apex::gateway
