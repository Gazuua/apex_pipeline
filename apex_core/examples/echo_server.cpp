/// Apex Pipeline - Echo Server Example (v0.5)
///
/// Minimal echo server using Server::run() blocking model.
/// Supports multicore via the [cores] CLI argument.
///
/// NOTE: The same EchoService class is intentionally duplicated in
/// multicore_echo_server.cpp to keep each example self-contained.
/// See multicore_echo_server.cpp for a multicore-focused variant with
/// hardware_concurrency() display and a higher default core count.
///
/// Usage: echo_server [port] [cores]
///   Default: port=9000, cores=1

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/tcp_binary_protocol.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/echo_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio/awaitable.hpp>

#include <spdlog/spdlog.h>

#include <cstdlib>

using namespace apex::core;
using boost::asio::awaitable;

class EchoService : public ServiceBase<EchoService> {
public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override {
        route<apex::messages::EchoRequest>(0x0001, &EchoService::on_echo);
    }

    awaitable<Result<void>> on_echo(SessionPtr session, uint16_t msg_id,
                            const apex::messages::EchoRequest* req) {
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
        // m-07: Explicitly discard [[nodiscard]] return value
        (void)co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        co_return ok();
    }
};

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    uint32_t cores = 1;
    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    }
    if (argc >= 3) {
        int c = std::atoi(argv[2]);
        if (c > 0 && c <= 256) cores = static_cast<uint32_t>(c);
    }

    auto config = AppConfig::defaults();
    config.server.num_cores = cores;
    config.server.heartbeat_timeout_ticks = 0;

    init_logging(config.logging);

    if (auto app = spdlog::get("app")) {
        app->info("=== Apex Pipeline Echo Server v0.5 ===");
        app->info("Port: {}, Cores: {}", port, cores);
    }

    Server(config.server)
        .listen<TcpBinaryProtocol>(port)
        .add_service<EchoService>()
        .run();

    if (auto app = spdlog::get("app")) {
        app->info("[Server] Done.");
    }
    shutdown_logging();
    return 0;
}
