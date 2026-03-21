// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/connection_handler.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/wire_header.hpp>

#include "../test_helpers.hpp"
#include "../test_mocks.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace apex::core;
using namespace std::chrono_literals;
using apex::test::build_test_frame;
using apex::test::make_socket_pair;
using apex::test::MockProtocol;
using apex::test::wait_for;

/// io_context를 백그라운드 스레드에서 실행하는 RAII guard.
/// 소멸 시 work_guard 해제 + stop + join.
struct IoRunner
{
    boost::asio::io_context& ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
    std::thread thread;

    explicit IoRunner(boost::asio::io_context& c)
        : ctx(c)
        , guard(c.get_executor())
        , thread([&c] { c.run(); })
    {}
    ~IoRunner()
    {
        guard.reset();
        ctx.stop();
        if (thread.joinable())
            thread.join();
    }
};

// TC1: accept_connection -- 세션 생성 확인
TEST(ConnectionHandlerTest, AcceptConnectionCreatesSession)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);

    auto [server_sock, client] = make_socket_pair(io_ctx);

    EXPECT_EQ(session_mgr.session_count(), 0u);
    handler.accept_connection(std::move(server_sock), io_ctx);

    // read_loop 코루틴이 스폰됨 — io_ctx 실행 전에도 세션은 생성됨
    EXPECT_EQ(session_mgr.session_count(), 1u);

    // active_sessions는 코루틴이 실행되어야 증가
    {
        IoRunner runner(io_ctx);
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 1u; }, 3000ms));

        client.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }
}

// TC2: 정상 프레임 수신 -- dispatch 호출 확인
TEST(ConnectionHandlerTest, NormalFrameDispatchesCorrectly)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};

    std::atomic<int> dispatch_count{0};
    uint32_t dispatched_msg_id = 0;
    std::vector<uint8_t> dispatched_payload;

    dispatcher.register_handler(
        0x0042,
        [&](SessionPtr, uint32_t msg_id, std::span<const uint8_t> payload) -> boost::asio::awaitable<Result<void>> {
            dispatch_count.fetch_add(1);
            dispatched_msg_id = msg_id;
            dispatched_payload.assign(payload.begin(), payload.end());
            co_return ok();
        });

    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    // 프레임 전송
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    auto frame = build_test_frame(0x0042, payload);
    {
        IoRunner runner(io_ctx);
        boost::asio::write(client, boost::asio::buffer(frame));

        ASSERT_TRUE(wait_for([&] { return dispatch_count.load() >= 1; }, 3000ms));
        client.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }

    EXPECT_EQ(dispatch_count.load(), 1);
    EXPECT_EQ(dispatched_msg_id, 0x0042);
    EXPECT_EQ(dispatched_payload, payload);
}

// TC3: 불완전 프레임 -- 추가 읽기 대기 (연결 유지)
TEST(ConnectionHandlerTest, IncompleteFrameWaitsForMoreData)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};

    std::atomic<int> dispatch_count{0};
    dispatcher.register_handler(0x0010,
                                [&](SessionPtr /*session*/, uint32_t /*msg_id*/,
                                    std::span<const uint8_t> /*payload*/) -> boost::asio::awaitable<Result<void>> {
                                    dispatch_count.fetch_add(1);
                                    co_return ok();
                                });

    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    // 헤더만 보내고 body는 아직 미전송
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    auto full_frame = build_test_frame(0x0010, payload);

    {
        IoRunner runner(io_ctx);

        // 헤더만 전송 (10바이트)
        boost::asio::write(client, boost::asio::buffer(full_frame.data(), WireHeader::SIZE));

        // 부재 증명: 헤더만 전송된 상태에서 dispatch가 호출되지 않아야 함.
        // TSAN/ASAN 환경에서는 timeout_multiplier로 대기 시간을 스케일링.
        std::this_thread::sleep_for(50ms * apex::test::timeout_multiplier());
        EXPECT_EQ(dispatch_count.load(), 0);
        EXPECT_GE(handler.active_sessions(), 1u);

        // 나머지 body 전송
        boost::asio::write(client, boost::asio::buffer(full_frame.data() + WireHeader::SIZE, payload.size()));

        ASSERT_TRUE(wait_for([&] { return dispatch_count.load() >= 1; }, 3000ms));

        client.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }

    EXPECT_EQ(dispatch_count.load(), 1);
}

// TC4: 유효하지 않은 프레임 -- 세션 종료
TEST(ConnectionHandlerTest, InvalidFrameClosesSession)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    {
        IoRunner runner(io_ctx);

        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 1u; }, 3000ms));

        // body_size가 MAX_BODY_SIZE를 초과하는 헤더 전송
        WireHeader bad_header{
            .msg_id = 0x0001,
            .body_size = WireHeader::MAX_BODY_SIZE + 1,
            .reserved = {},
        };
        auto hdr_bytes = bad_header.serialize();
        boost::asio::write(client, boost::asio::buffer(std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

        // 세션이 닫히면 active_sessions가 0으로 돌아감
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0; }, 3000ms));

        client.close();
    }
}

// TC5: 클라이언트 연결 끊김 -- 세션 정리
TEST(ConnectionHandlerTest, ClientDisconnectCleansUpSession)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    {
        IoRunner runner(io_ctx);

        // 세션이 활성화될 때까지 대기
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 1u; }, 3000ms));

        // 클라이언트 연결 끊기
        client.close();

        // read_loop가 종료되고 세션이 정리됨
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0; }, 3000ms));
    }
}

// TC6: dispatch 실패 -- 에러 응답 전송
TEST(ConnectionHandlerTest, DispatchFailureSendsErrorResponse)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};
    // HandlerNotFound를 유도 — 핸들러 미등록 msg_id로 프레임 전송
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    {
        IoRunner runner(io_ctx);

        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 1u; }, 3000ms));

        // 등록되지 않은 msg_id 0x9999로 프레임 전송
        auto frame = build_test_frame(0x9999);
        boost::asio::write(client, boost::asio::buffer(frame));

        // 에러 응답 프레임 수신 대기
        std::vector<uint8_t> hdr_buf(WireHeader::SIZE);
        boost::system::error_code ec;
        boost::asio::read(client, boost::asio::buffer(hdr_buf), ec);
        ASSERT_FALSE(ec) << "Error reading response: " << ec.message();

        auto resp_header = WireHeader::parse(hdr_buf);
        ASSERT_TRUE(resp_header.has_value());
        EXPECT_EQ(resp_header->msg_id, 0x9999);
        EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

        client.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }
}

// TC7: 다중 프레임 연속 처리
TEST(ConnectionHandlerTest, MultipleFramesProcessedSequentially)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};

    std::atomic<int> dispatch_count{0};
    std::vector<uint32_t> dispatched_ids;
    std::mutex ids_mutex;

    auto make_handler = [&](uint32_t msg_id) {
        return [&, msg_id](SessionPtr, uint32_t id, std::span<const uint8_t>) -> boost::asio::awaitable<Result<void>> {
            {
                std::lock_guard lock(ids_mutex);
                dispatched_ids.push_back(id);
            }
            dispatch_count.fetch_add(1);
            co_return ok();
        };
    };

    dispatcher.register_handler(0x0001, make_handler(0x0001));
    dispatcher.register_handler(0x0002, make_handler(0x0002));
    dispatcher.register_handler(0x0003, make_handler(0x0003));

    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    {
        IoRunner runner(io_ctx);

        // 3개 프레임을 한 번에 전송
        auto frame1 = build_test_frame(0x0001);
        auto frame2 = build_test_frame(0x0002);
        auto frame3 = build_test_frame(0x0003);

        std::vector<uint8_t> batch;
        batch.insert(batch.end(), frame1.begin(), frame1.end());
        batch.insert(batch.end(), frame2.begin(), frame2.end());
        batch.insert(batch.end(), frame3.begin(), frame3.end());
        boost::asio::write(client, boost::asio::buffer(batch));

        ASSERT_TRUE(wait_for([&] { return dispatch_count.load() >= 3; }, 3000ms));

        client.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }

    EXPECT_EQ(dispatch_count.load(), 3);
    std::lock_guard lock(ids_mutex);
    ASSERT_EQ(dispatched_ids.size(), 3u);
    EXPECT_EQ(dispatched_ids[0], 0x0001);
    EXPECT_EQ(dispatched_ids[1], 0x0002);
    EXPECT_EQ(dispatched_ids[2], 0x0003);
}

// TC8: recv_buffer 오버플로 -- 세션 종료
// read_loop에서 writable().empty() 분기를 테스트한다.
// 매우 작은 recv_buf_capacity(32)로 SessionManager를 생성하고,
// body_size > capacity인 프레임 헤더를 보내 버퍼를 채운다.
TEST(ConnectionHandlerTest, RecvBufferOverflowClosesSession)
{
    boost::asio::io_context io_ctx;
    // recv_buf_capacity=32 — 헤더(12) + body 20바이트로 꽉 참
    SessionManager session_mgr(0, 0, 8, 32);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    {
        IoRunner runner(io_ctx);

        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 1u; }, 3000ms));

        // body_size=100으로 헤더를 보낸다. 실제로 100바이트를 다 보내면
        // try_decode는 InsufficientData를 계속 반환하고, 결국 recv_buffer가 꽉 차서
        // writable().empty() → session close.
        WireHeader header{
            .msg_id = 0x0001,
            .body_size = 100, // 32바이트 버퍼에 12+100 = 112바이트 필요
            .reserved = {},
        };
        auto hdr_bytes = header.serialize();
        boost::asio::write(client, boost::asio::buffer(std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

        // 나머지 body 데이터를 계속 전송해서 버퍼를 채운다
        std::vector<uint8_t> filler(100, 0xAA);
        boost::system::error_code ec;
        boost::asio::write(client, boost::asio::buffer(filler), ec);
        // 서버가 close하면 write에서 에러날 수 있지만, 그 전에 버퍼가 차면 OK

        // 세션이 close되어야 함 (writable empty 또는 InvalidMessage)
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));

        client.close();
    }
}

// TC9: tcp_nodelay=false 분기 테스트
TEST(ConnectionHandlerTest, TcpNodelayFalseSkipsOptionSet)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = false};
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);
    auto [server_sock, client] = make_socket_pair(io_ctx);
    handler.accept_connection(std::move(server_sock), io_ctx);

    {
        IoRunner runner(io_ctx);

        // 세션 정상 생성 확인
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 1u; }, 3000ms));

        // tcp_nodelay 옵션이 설정되지 않았으므로 기본값 유지
        // (crash 없이 정상 동작하면 분기 커버됨)
        client.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }
}

// TC10: active_sessions 카운트 정확성
TEST(ConnectionHandlerTest, ActiveSessionsCountAccurate)
{
    boost::asio::io_context io_ctx;
    SessionManager session_mgr(0, 0, 8, 8192);
    MessageDispatcher dispatcher;
    ConnectionHandlerConfig config{.tcp_nodelay = true};
    ConnectionHandler<MockProtocol> handler(session_mgr, dispatcher, config);

    EXPECT_EQ(handler.active_sessions(), 0u);

    auto [s1, c1] = make_socket_pair(io_ctx);
    auto [s2, c2] = make_socket_pair(io_ctx);

    handler.accept_connection(std::move(s1), io_ctx);
    handler.accept_connection(std::move(s2), io_ctx);

    {
        IoRunner runner(io_ctx);

        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() >= 2u; }, 3000ms));
        EXPECT_EQ(handler.active_sessions(), 2u);

        // 한 클라이언트 끊기
        c1.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() <= 1u; }, 3000ms));
        EXPECT_EQ(handler.active_sessions(), 1u);

        // 나머지 클라이언트 끊기
        c2.close();
        ASSERT_TRUE(wait_for([&] { return handler.active_sessions() == 0u; }, 3000ms));
    }
}
