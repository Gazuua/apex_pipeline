/// Apex Pipeline - Chat Server Example (v0.5)
///
/// 단일 코어 브로드캐스트 시연.
/// 멀티코어 크로스코어 브로드캐스트는 Redis Pub/Sub 경유 (v0.3.0+).
///
/// Usage: chat_server [port]
///   Default: port=9001

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/tcp_binary_protocol.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/chat_message_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio/awaitable.hpp>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <vector>

using namespace apex::core;
using boost::asio::awaitable;

/// Single-core broadcast chat service.
/// For multicore broadcast, use Redis Pub/Sub (v0.3.0+).
class ChatService : public ServiceBase<ChatService> {
public:
    ChatService(SessionManager& mgr) : ServiceBase("chat"), session_mgr_(mgr) {}

    void on_start() override {
        route<apex::messages::ChatMessage>(0x0100, &ChatService::on_chat);
    }

    awaitable<Result<void>> on_chat(SessionPtr sender, uint32_t msg_id,
                            const apex::messages::ChatMessage* msg) {
        if (!msg || !msg->content()) co_return ok();

        flatbuffers::FlatBufferBuilder builder(256);
        auto content = builder.CreateString(msg->content()->str());
        auto broadcast = apex::messages::CreateChatMessage(
            builder, sender->id(), content);
        builder.Finish(broadcast);

        WireHeader header{
            .msg_id = msg_id,
            .body_size = static_cast<uint32_t>(builder.GetSize())
        };

        auto payload_span = std::span<const uint8_t>{
            builder.GetBufferPointer(), builder.GetSize()};

        std::vector<SessionPtr> sessions;
        session_mgr_.for_each([&](SessionPtr s) {
            sessions.push_back(s);
        });
        for (auto& s : sessions) {
            if (!(co_await s->async_send(header, payload_span)).has_value()) {
                // Send failed — peer likely disconnected. Session cleanup
                // is handled by the read_loop, so we just skip here.
            }
        }
        co_return ok();
    }

private:
    SessionManager& session_mgr_;
};

int main(int argc, char* argv[]) {
    uint16_t port = 9001;
    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    }

    auto config = AppConfig::defaults();
    config.server.num_cores = 1;
    config.server.heartbeat_timeout_ticks = 0;

    init_logging(config.logging);

    if (auto app = spdlog::get("app")) {
        app->info("=== Apex Pipeline Chat Server v0.5 ===");
        app->info("Port: {} (single-core broadcast)", port);
    }

    Server(config.server)
        .listen<TcpBinaryProtocol>(port)
        .add_service_factory([](PerCoreState& state) {
            return std::make_unique<ChatService>(state.session_mgr);
        })
        .run();

    if (auto app = spdlog::get("app")) {
        app->info("[Chat] Done.");
    }
    shutdown_logging();
    return 0;
}
