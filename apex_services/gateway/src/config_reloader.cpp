#include <apex/gateway/config_reloader.hpp>

#include <spdlog/spdlog.h>

namespace apex::gateway {

ConfigReloader::ConfigReloader(boost::asio::io_context& io_ctx,
                               std::string config_path,
                               RouteUpdateCallback on_update)
    : config_path_(std::move(config_path))
    , on_update_(std::move(on_update))
    , watcher_(std::make_unique<FileWatcher>(
          io_ctx, config_path_,
          [this](const std::string& path) { on_file_changed(path); })) {}

void ConfigReloader::start() {
    watcher_->start();
}

void ConfigReloader::stop() {
    watcher_->stop();
}

void ConfigReloader::on_file_changed(const std::string& path) {
    spdlog::info("Config file changed, reloading: {}", path);

    auto config = parse_gateway_config(path);
    if (!config) {
        spdlog::error("Failed to parse updated config, keeping old routes");
        return;
    }

    auto table = RouteTable::build(config->routes);
    if (!table) {
        spdlog::error("Failed to build route table from updated config");
        return;
    }

    auto ptr = std::make_shared<const RouteTable>(std::move(*table));
    spdlog::info("Route table reloaded: {} entries", ptr->size());

    if (on_update_) {
        on_update_(std::move(ptr));
    }
}

} // namespace apex::gateway
