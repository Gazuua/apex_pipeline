#pragma once

#include <apex/gateway/file_watcher.hpp>
#include <apex/gateway/route_table.hpp>
#include <apex/gateway/gateway_config_parser.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apex::gateway {

/// TOML file change detection -> RouteTable atomic replacement.
/// When FileWatcher detects change:
/// 1. Re-parse TOML
/// 2. Build new RouteTable + validate
/// 3. Distribute new table to all per-core MessageRouters
class ConfigReloader {
public:
    using RouteUpdateCallback = std::function<void(RouteTablePtr)>;

    ConfigReloader(boost::asio::io_context& io_ctx,
                   std::string config_path,
                   RouteUpdateCallback on_update);

    /// Start watching.
    void start();

    /// Stop watching.
    void stop();

private:
    void on_file_changed(const std::string& path);

    std::string config_path_;
    RouteUpdateCallback on_update_;
    std::unique_ptr<FileWatcher> watcher_;
};

} // namespace apex::gateway
