#include <apex/gateway/config_reloader.hpp>

#include <spdlog/spdlog.h>

namespace apex::gateway
{

apex::shared::rate_limit::EndpointRateConfig to_endpoint_rate_config(const RateLimitEndpointConfig& src)
{
    apex::shared::rate_limit::EndpointRateConfig dst;
    dst.default_limit = src.default_limit;
    dst.window_size = std::chrono::seconds{src.window_size_seconds};
    for (const auto& [msg_id, limit] : src.overrides)
    {
        dst.overrides.emplace(msg_id, limit);
    }
    return dst;
}

ConfigReloader::ConfigReloader(boost::asio::io_context& io_ctx, std::string config_path,
                               std::vector<std::reference_wrapper<boost::asio::io_context>> core_io_contexts,
                               RouteUpdateCallback on_route_update, RateLimitUpdateCallback on_rate_limit_update)
    : config_path_(std::move(config_path))
    , core_io_contexts_(std::move(core_io_contexts))
    , on_route_update_(std::move(on_route_update))
    , on_rate_limit_update_(std::move(on_rate_limit_update))
    , watcher_(std::make_unique<FileWatcher>(io_ctx, config_path_,
                                             [this](const std::string& path) { on_file_changed(path); }))
{}

ConfigReloader::ConfigReloader(boost::asio::io_context& io_ctx, std::string config_path,
                               RouteUpdateCallback on_route_update, RateLimitUpdateCallback on_rate_limit_update)
    : ConfigReloader(io_ctx, std::move(config_path), {}, std::move(on_route_update), std::move(on_rate_limit_update))
{}

void ConfigReloader::start()
{
    watcher_->start();
}

void ConfigReloader::stop()
{
    watcher_->stop();
}

void ConfigReloader::on_file_changed(const std::string& path)
{
    spdlog::info("Config file changed, reloading: {}", path);

    auto config = parse_gateway_config(path);
    if (!config)
    {
        spdlog::error("Failed to parse updated config, keeping old settings");
        return;
    }

    // Update route table
    auto table = RouteTable::build(config->routes);
    if (!table)
    {
        spdlog::error("Failed to build route table from updated config");
        return;
    }

    auto ptr = std::make_shared<const RouteTable>(std::move(*table));
    spdlog::info("Route table reloaded: {} entries", ptr->size());

    // Update rate limit config
    auto rl_config = config->rate_limit;

    if (core_io_contexts_.empty())
    {
        // Legacy path: fire directly on watcher thread
        if (on_route_update_)
        {
            on_route_update_(ptr);
        }
        if (on_rate_limit_update_)
        {
            spdlog::info("Rate limit config reloaded");
            on_rate_limit_update_(rl_config);
        }
    }
    else
    {
        // Per-core post: dispatch to each core's io_context thread.
        // This ensures shared-nothing — each core updates its own state
        // without cross-thread mutation.
        for (auto& core_io : core_io_contexts_)
        {
            if (on_route_update_)
            {
                boost::asio::post(core_io.get(), [cb = on_route_update_, t = ptr]() { cb(t); });
            }
            if (on_rate_limit_update_)
            {
                boost::asio::post(core_io.get(), [cb = on_rate_limit_update_, rl = rl_config]() { cb(rl); });
            }
        }
        spdlog::info("Config update posted to {} core(s)", core_io_contexts_.size());
    }
}

} // namespace apex::gateway
