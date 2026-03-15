#include <apex/gateway/gateway_service.hpp>

namespace apex::gateway {

GatewayService::GatewayService(const GatewayConfig& config)
    : ServiceBase("gateway")
    , config_(config)
    , logger_(spdlog::default_logger()->clone("gateway")) {
    logger_->info("GatewayService created (routes: {})", config_.routes.size());
}

GatewayService::~GatewayService() = default;

} // namespace apex::gateway
