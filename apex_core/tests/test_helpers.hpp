#pragma once

/// 공용 테스트 헬퍼 -- run_coro, make_socket_pair
/// 여러 테스트 파일에서 중복 정의되었던 유틸리티를 통합.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <future>
#include <thread>
#include <utility>

namespace apex::test
{

/// 코루틴을 동기적으로 실행하고 결과를 반환하는 테스트 헬퍼.
template <typename T> T run_coro(boost::asio::io_context& ctx, boost::asio::awaitable<T> aw)
{
    auto future = boost::asio::co_spawn(ctx, std::move(aw), boost::asio::use_future);
    ctx.run();
    ctx.restart();
    return future.get();
}

/// void 특수화
inline void run_coro(boost::asio::io_context& ctx, boost::asio::awaitable<void> aw)
{
    auto future = boost::asio::co_spawn(ctx, std::move(aw), boost::asio::use_future);
    ctx.run();
    ctx.restart();
    future.get();
}

/// 테스트용 연결된 소켓 쌍 생성.
inline std::pair<boost::asio::ip::tcp::socket, boost::asio::ip::tcp::socket>
make_socket_pair(boost::asio::io_context& ctx)
{
    using tcp = boost::asio::ip::tcp;
    tcp::acceptor acceptor(ctx, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    tcp::socket client(ctx);
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    auto server = acceptor.accept();

    return {std::move(server), std::move(client)};
}

/// TSAN/ASAN 환경에서는 타임아웃을 자동 확대
constexpr int timeout_multiplier() noexcept
{
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
    return 10;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    return 10;
#else
    return 1;
#endif
#else
    return 1;
#endif
}

/// 조건 대기 헬퍼: pred가 true를 반환할 때까지 1ms 간격으로 폴링.
/// timeout 내에 충족되면 true, 초과 시 false 반환.
template <typename Pred> bool wait_for(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
{
    auto effective_timeout = timeout * timeout_multiplier();
    auto deadline = std::chrono::steady_clock::now() + effective_timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace apex::test
