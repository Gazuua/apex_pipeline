#pragma once

#include <apex/gateway/gateway_config.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/session.hpp>

#include <spdlog/spdlog.h>

#include <memory>

namespace apex::gateway {

/// Gateway service.
/// Does not register message handlers -- Gateway is a generic proxy,
/// so it uses its own routing logic instead of MessageDispatcher.
///
/// Gateway core roles:
/// 1. Client WireHeader -> Kafka Envelope conversion + produce
/// 2. Kafka response -> WireHeader conversion + client delivery
/// 3. Redis Pub/Sub -> broadcast delivery
class GatewayService
    : public apex::core::ServiceBase<GatewayService> {
public:
    explicit GatewayService(const GatewayConfig& config);
    ~GatewayService();

    /// ServiceBase interface -- handler registration.
    /// Gateway routes all msg_ids itself, so no individual handler registration.
    void setup_handlers() {}

private:
    GatewayConfig config_;
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace apex::gateway
