/// Apex Pipeline - Multicore Echo Server Example (v0.2.4)
///
/// io_context-per-core 아키텍처 시연.
/// 각 코어가 독립된 EchoService 인스턴스를 소유.
///
/// Usage: multicore_echo_server [port] [cores]
///   Default: port=9000, cores=4

#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/echo_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio/awaitable.hpp>

#include <cstdlib>
#include <iostream>
#include <thread>

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
        co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        co_return ok();
    }
};

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    uint32_t cores = 4;
    if (argc >= 2) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    }
    if (argc >= 3) {
        int c = std::atoi(argv[2]);
        if (c > 0 && c <= 256) cores = static_cast<uint32_t>(c);
    }

    auto hw = std::thread::hardware_concurrency();
    std::cout << "=== Apex Pipeline Multicore Echo Server v0.2.4 ===\n"
              << "Port: " << port << ", Cores: " << cores
              << " (hardware: " << hw << ")\n"
              << "Architecture: io_context-per-core (shared-nothing)\n\n";

    Server({.port = port, .num_cores = cores, .heartbeat_timeout_ticks = 0})
        .add_service<EchoService>()
        .run();

    std::cout << "[Server] Done.\n";
    return 0;
}
