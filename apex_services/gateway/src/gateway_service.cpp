// NOMINMAX must precede any Windows header to prevent min/max macro
// conflicts with std::numeric_limits in FlatBuffers headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <apex/gateway/gateway_service.hpp>
#include <apex/gateway/jwt_verifier.hpp>
#include <apex/gateway/jwt_blacklist.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>

#include <apex/core/wire_header.hpp>
#include <flatbuffers/flatbuffers.h>

namespace apex::gateway {

GatewayService::GatewayService(const GatewayConfig& config, Dependencies deps)
    : ServiceBase("gateway")
    , config_(config)
    , logger_(spdlog::default_logger()->clone("gateway"))
    , pipeline_(config_, deps.jwt_verifier, deps.jwt_blacklist)
    , router_(deps.kafka, deps.route_table, static_cast<uint16_t>(deps.core_id),
              config_.kafka_response_topic)
    , pending_requests_(config_.max_pending_per_core, config_.request_timeout)
{
    logger_->info("GatewayService created (core_id={}, routes={})",
                  deps.core_id, config_.routes.size());
}

GatewayService::~GatewayService() = default;

void GatewayService::on_start() {
    // Default handler — routes all messages.
    // System messages (e.g., AuthenticateSession) are handled inline;
    // service messages go through pipeline + Kafka routing.
    dispatcher().set_default_handler(
        [this](apex::core::SessionPtr session, uint32_t msg_id,
               std::span<const uint8_t> payload)
            -> boost::asio::awaitable<apex::core::Result<void>> {

            // System message: AuthenticateSession (msg_id=3)
            // Client sends JWT token after login; bind it to the session.
            if (msg_id == 3) {
                if (!payload.empty()) {
                    auto* root = flatbuffers::GetRoot<flatbuffers::Table>(payload.data());
                    if (root) {
                        auto* token = root->GetPointer<const flatbuffers::String*>(4);
                        if (token && token->size() > 0) {
                            auto& state = auth_states_[session->id()];
                            state.token = token->str();
                            logger_->info("JWT bound to session {}", session->id());
                        }
                    }
                }
                co_return apex::core::ok();
            }

            // Service messages — pipeline + Kafka routing
            co_return co_await handle_request(std::move(session), msg_id, payload);
        });
}

void GatewayService::on_stop() {
    dispatcher().clear_default_handler();
    auth_states_.clear();
}

boost::asio::awaitable<apex::core::Result<void>>
GatewayService::handle_request(apex::core::SessionPtr session,
                                uint32_t msg_id,
                                std::span<const uint8_t> payload) {
    // 1. Get or create per-session auth state
    auto& state = auth_states_[session->id()];

    // 2. Resolve remote IP from socket
    std::string remote_ip;
    {
        boost::system::error_code ec;
        auto ep = session->socket().remote_endpoint(ec);
        if (!ec) {
            remote_ip = ep.address().to_string();
        } else {
            remote_ip = "unknown";
        }
    }

    // 3. Build WireHeader for pipeline + router
    apex::core::WireHeader header;
    header.msg_id = msg_id;
    header.body_size = static_cast<uint32_t>(payload.size());

    // 4. Run gateway pipeline (rate limit + auth)
    auto pipeline_result = co_await pipeline_.process(session, header, state, remote_ip);
    if (!pipeline_result) {
        logger_->error("handle_request: pipeline failed for msg_id={}, error={}",
                       msg_id, static_cast<int>(pipeline_result.error()));
        co_return std::unexpected(pipeline_result.error());
    }

    // 5. Route to Kafka via MessageRouter
    auto corr_id = router_.generate_corr_id();

    // 6. Register pending request (for response correlation)
    auto pending_result = pending_requests_.insert(
        corr_id, session->id(), msg_id);
    if (!pending_result) {
        co_return std::unexpected(pending_result.error());
    }

    // 7. Produce to Kafka
    auto route_result = router_.route(
        session, header, payload, state.user_id, corr_id);
    if (!route_result) {
        logger_->error("handle_request: route failed for msg_id={}, error={}",
                       msg_id, static_cast<int>(route_result.error()));
        // Remove pending entry on failure
        (void)pending_requests_.extract(corr_id);
        co_return std::unexpected(route_result.error());
    }
    logger_->info("handle_request: msg_id={} routed successfully (corr_id={})",
                  msg_id, corr_id);

    co_return apex::core::ok();
}

} // namespace apex::gateway
