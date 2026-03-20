#include <apex/core/session.hpp>
#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

using namespace apex::core;
using boost::asio::ip::tcp;

static void BM_Session_Create(benchmark::State& state)
{
    boost::asio::io_context io_ctx;
    tcp::acceptor acc(io_ctx, tcp::endpoint(tcp::v4(), 0));
    auto port = acc.local_endpoint().port();

    for (auto _ : state)
    {
        tcp::socket client(io_ctx);
        client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
        auto server_sock = acc.accept();

        SessionPtr session(new Session(make_session_id(1), std::move(server_sock), 0, 8192));
        benchmark::DoNotOptimize(session.get());

        client.close();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Session_Create);

static void BM_SessionPtr_Copy(benchmark::State& state)
{
    boost::asio::io_context io_ctx;
    tcp::acceptor acc(io_ctx, tcp::endpoint(tcp::v4(), 0));
    auto port = acc.local_endpoint().port();
    tcp::socket client(io_ctx);
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    auto server_sock = acc.accept();

    SessionPtr session(new Session(make_session_id(1), std::move(server_sock), 0, 8192));
    for (auto _ : state)
    {
        auto copy = session;
        benchmark::DoNotOptimize(copy.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SessionPtr_Copy);
