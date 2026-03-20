#include "../bench_helpers.hpp"

#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>

#include <benchmark/benchmark.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

using namespace apex::core;
using boost::asio::ip::tcp;

// Measures synchronous TCP send/recv round-trip through Session
static void BM_Session_EchoRoundTrip(benchmark::State& state)
{
    auto payload_size = static_cast<size_t>(state.range(0));
    boost::asio::io_context io_ctx;

    auto [server, client] = apex::bench::make_socket_pair(io_ctx);
    SessionPtr session(new Session(make_session_id(1), std::move(server), 0, payload_size * 4));

    auto frame = apex::bench::build_frame(0x0001, payload_size);
    std::vector<uint8_t> recv_buf(frame.size());

    for (auto _ : state)
    {
        // Client sends
        boost::asio::write(client, boost::asio::buffer(frame));

        // Server reads
        boost::asio::read(session->socket(), boost::asio::buffer(recv_buf.data(), frame.size()));
        benchmark::DoNotOptimize(recv_buf.data());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(frame.size()));
}
BENCHMARK(BM_Session_EchoRoundTrip)->Range(64, 4096);
