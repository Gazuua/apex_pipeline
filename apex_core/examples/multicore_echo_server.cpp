/// Apex Pipeline - Multicore Echo Server Example (v0.5)
///
/// Demonstrates the io_context-per-core (shared-nothing) architecture.
/// Each core owns an independent EchoService instance.
///
/// NOTE: The EchoService class is intentionally duplicated from
/// echo_server.cpp to keep each example self-contained.
/// This variant defaults to 4 cores and displays hardware_concurrency().
///
/// Usage: multicore_echo_server [port] [cores]
///   Default: port=9000, cores=4

#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/tcp_binary_protocol.hpp>
#include <apex/core/wire_header.hpp>

#include <flatbuffers/flatbuffers.h>
#include <generated/echo_generated.h>

#include <boost/asio/awaitable.hpp>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <thread>

using namespace apex::core;
using boost::asio::awaitable;

class EchoService : public ServiceBase<EchoService>
{
  public:
    EchoService()
        : ServiceBase("echo")
    {}

    void on_start() override
    {
        route<apex::messages::EchoRequest>(0x0001, &EchoService::on_echo);
    }

    awaitable<Result<void>> on_echo(SessionPtr session, uint32_t msg_id, const apex::messages::EchoRequest* req)
    {
        if (!req || !req->data())
            co_return ok();

        flatbuffers::FlatBufferBuilder builder(256);
        auto data_vec = builder.CreateVector(req->data()->data(), req->data()->size());
        auto resp = apex::messages::CreateEchoResponse(builder, data_vec);
        builder.Finish(resp);

        WireHeader header{.msg_id = msg_id, .body_size = static_cast<uint32_t>(builder.GetSize()), .reserved = {}};
        // m-07: Explicitly discard [[nodiscard]] return value
        (void)co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        co_return ok();
    }
};

int main(int argc, char* argv[])
{
    uint16_t port = 9000;
    uint32_t cores = 4;
    if (argc >= 2)
    {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535)
            port = static_cast<uint16_t>(p);
    }
    if (argc >= 3)
    {
        int c = std::atoi(argv[2]);
        if (c > 0 && c <= 256)
            cores = static_cast<uint32_t>(c);
    }

    auto config = AppConfig::defaults();
    config.server.num_cores = cores;
    config.server.heartbeat_timeout_ticks = 0;

    init_logging(config.logging);

    auto hw = std::thread::hardware_concurrency();
    if (auto app = spdlog::get("app"))
    {
        app->info("=== Apex Pipeline Multicore Echo Server v0.5 ===");
        app->info("Port: {}, Cores: {} (hardware: {})", port, cores, hw);
        app->info("Architecture: io_context-per-core (shared-nothing)");
    }

    Server(config.server).listen<TcpBinaryProtocol>(port).add_service<EchoService>().run();

    if (auto app = spdlog::get("app"))
    {
        app->info("[Server] Done.");
    }
    shutdown_logging();
    return 0;
}
