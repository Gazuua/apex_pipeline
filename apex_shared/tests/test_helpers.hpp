// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

/// apex_shared 테스트 공용 헬퍼 — run_coro
/// apex_core/tests/test_helpers.hpp와 동일 인터페이스를 제공하되,
/// apex_shared 테스트의 CMake 타겟이 apex_core 테스트에 의존하지 않으므로 별도 정의.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

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

/// callable(lambda) 오버로드 — co_spawn + detached 방식.
/// test_circuit_breaker.cpp 등에서 사용하는 패턴.
template <typename Fn> void run_coro(boost::asio::io_context& ctx, Fn&& fn)
{
    ctx.restart();
    boost::asio::co_spawn(ctx, std::forward<Fn>(fn), boost::asio::detached);
    ctx.run();
}

} // namespace apex::test
