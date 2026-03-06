#pragma once

/// 공용 테스트 헬퍼 -- run_coro, make_socket_pair
/// 여러 테스트 파일에서 중복 정의되었던 유틸리티를 통합.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <future>
#include <utility>

namespace apex::test {

/// 코루틴을 동기적으로 실행하고 결과를 반환하는 테스트 헬퍼.
template <typename T>
T run_coro(boost::asio::io_context& ctx, boost::asio::awaitable<T> aw) {
    auto future = boost::asio::co_spawn(ctx, std::move(aw), boost::asio::use_future);
    ctx.run();
    ctx.restart();
    return future.get();
}

/// void 특수화
inline void run_coro(boost::asio::io_context& ctx, boost::asio::awaitable<void> aw) {
    auto future = boost::asio::co_spawn(ctx, std::move(aw), boost::asio::use_future);
    ctx.run();
    ctx.restart();
    future.get();
}

/// 테스트용 연결된 소켓 쌍 생성.
inline std::pair<boost::asio::ip::tcp::socket, boost::asio::ip::tcp::socket>
make_socket_pair(boost::asio::io_context& ctx) {
    using tcp = boost::asio::ip::tcp;
    tcp::acceptor acceptor(ctx, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    tcp::socket client(ctx);
    client.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), port));
    auto server = acceptor.accept();

    return {std::move(server), std::move(client)};
}

} // namespace apex::test
