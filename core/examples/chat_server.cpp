/// Apex Pipeline - Chat Server Example (v0.2.1)
///
/// 크로스 세션 브로드캐스트 시연.
/// SessionPtr + SessionManager.for_each() 활용.
/// 핸들러 코루틴 전환 완료.
///
/// Usage: chat_server [port]
///   Default: port=9001

#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/chat_message_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace apex::core;
using boost::asio::awaitable;

class ChatService : public ServiceBase<ChatService> {
public:
    ChatService(SessionManager& mgr) : ServiceBase("chat"), session_mgr_(mgr) {}

    void on_start() override {
        route<apex::messages::ChatMessage>(0x0100, &ChatService::on_chat);
        std::cout << "[ChatService] Started. Handler: chat(0x0100)\n";
    }

    void on_stop() override {
        std::cout << "[ChatService] Stopped. Total: " << msg_count_ << " messages\n";
    }

    awaitable<void> on_chat(SessionPtr sender, uint16_t msg_id,
                            const apex::messages::ChatMessage* msg) {
        ++msg_count_;
        if (!msg || !msg->content()) co_return;

        std::cout << "[Chat] Session " << sender->id()
                  << ": " << msg->content()->str() << "\n";

        // 브로드캐스트: 모든 세션에 전송
        flatbuffers::FlatBufferBuilder builder(256);
        auto content = builder.CreateString(msg->content()->str());
        auto broadcast = apex::messages::CreateChatMessage(
            builder, sender->id(), content);
        builder.Finish(broadcast);

        WireHeader header{
            .msg_id = msg_id,
            .body_size = static_cast<uint32_t>(builder.GetSize())
        };

        // for_each는 동기 콜백이므로, 세션 목록을 수집 후 코루틴에서 순회
        std::vector<SessionPtr> sessions;
        session_mgr_.for_each([&](SessionPtr s) {
            sessions.push_back(s);
        });
        for (auto& s : sessions) {
            co_await s->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        }
    }

private:
    SessionManager& session_mgr_;
    int msg_count_{0};
};

int main(int argc, char* argv[]) {
    uint16_t port = 9001;
    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    }

    std::cout << "=== Apex Pipeline Chat Server v0.2.1 ===\n"
              << "Port: " << port << "\n\n";

    boost::asio::io_context io_ctx;

    Server server(io_ctx, {.port = port, .heartbeat_timeout_ticks = 300});
    server.add_service<ChatService>(server.session_manager());
    server.start();

    std::cout << "[Server] Listening on port " << server.port() << "\n";

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) {
        std::cout << "\n[Server] Shutting down...\n";
        server.stop();
    });

    io_ctx.run();
    std::cout << "[Server] Done.\n";
    return 0;
}
