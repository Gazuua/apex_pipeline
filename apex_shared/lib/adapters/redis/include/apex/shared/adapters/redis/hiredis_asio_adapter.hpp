// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

struct redisAsyncContext;

namespace apex::shared::adapters::redis
{

/// hiredis redisAsyncContext의 fd를 Boost.Asio 소켓에 등록하여
/// hiredis의 이벤트 콜백을 Asio 이벤트 루프에 위임하는 어댑터.
///
/// 패턴: redisAsyncContext -> ev 콜백 등록 -> Asio socket async_wait
///
/// 하나의 redisAsyncContext에 대해 하나의 HiredisAsioAdapter 인스턴스.
class HiredisAsioAdapter
{
  public:
    /// redisAsyncContext를 Asio io_context에 바인딩.
    /// 내부적으로 fd를 추출하여 tcp::socket에 assign한다.
    HiredisAsioAdapter(boost::asio::io_context& io_ctx, redisAsyncContext* ac);
    ~HiredisAsioAdapter();

    // Non-copyable, non-movable (소켓 소유)
    HiredisAsioAdapter(const HiredisAsioAdapter&) = delete;
    HiredisAsioAdapter& operator=(const HiredisAsioAdapter&) = delete;
    HiredisAsioAdapter(HiredisAsioAdapter&&) = delete;
    HiredisAsioAdapter& operator=(HiredisAsioAdapter&&) = delete;

  private:
    // hiredis ev 콜백으로 등록되는 static 함수들
    static void on_add_read(void* privdata);
    static void on_del_read(void* privdata);
    static void on_add_write(void* privdata);
    static void on_del_write(void* privdata);
    static void on_cleanup(void* privdata);

    void handle_read();
    void handle_write();

    boost::asio::ip::tcp::socket socket_;
    redisAsyncContext* ac_; ///< 비소유. 생명주기는 RedisConnection이 관리.
    bool read_requested_ = false;
    bool write_requested_ = false;
    bool cleaned_up_ = false;
};

} // namespace apex::shared::adapters::redis
