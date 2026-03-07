/// Apex Pipeline - Echo Server Example (v0.2.1)
///
/// Server 클래스 기반. 핸들러 코루틴 전환 완료.
///
/// Usage: echo_server [port]
///   Default: port=9000

#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/echo_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdlib>
#include <iostream>

using namespace apex::core;
using boost::asio::awaitable;

class EchoService : public ServiceBase<EchoService> {
public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override {
        route<apex::messages::EchoRequest>(0x0001, &EchoService::on_echo);
        std::cout << "[EchoService] Started. Handler: echo(0x0001)\n";
    }

    void on_stop() override {
        std::cout << "[EchoService] Stopped. Total: " << count_ << " echo\n";
    }

    awaitable<Result<void>> on_echo(SessionPtr session, uint16_t msg_id,
                            const apex::messages::EchoRequest* req) {
        ++count_;
        if (!req || !req->data()) co_return ok();

        flatbuffers::FlatBufferBuilder builder(256);
        auto data_vec = builder.CreateVector(
            req->data()->data(), req->data()->size());
        auto resp = apex::messages::CreateEchoResponse(builder, data_vec);
        builder.Finish(resp);

        WireHeader header{
            .msg_id = msg_id,
            .body_size = static_cast<uint32_t>(builder.GetSize())
        };
        co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        co_return ok();
    }

private:
    int count_{0};
};

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    }

    std::cout << "=== Apex Pipeline Echo Server v0.2.1 ===\n"
              << "Port: " << port << "\n\n";

    boost::asio::io_context io_ctx;

    Server server(io_ctx, {.port = port, .heartbeat_timeout_ticks = 0});
    server.add_service<EchoService>();
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
