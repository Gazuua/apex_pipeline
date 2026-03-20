// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "e2e_test_fixture.hpp"

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstdlib>
#include <thread>
#include <vector>

namespace apex::e2e
{

class E2EStressProtocolFixture : public E2ETestFixture
{
  protected:
    int stress_messages_ = get_env_int("E2E_STRESS_MESSAGES", 20);

    /// Raw TCP 소켓으로 Gateway에 연결 (TcpClient 없이 직접 제어)
    boost::asio::ip::tcp::socket connect_raw()
    {
        boost::asio::ip::tcp::socket sock(io_ctx_);
        boost::asio::ip::tcp::resolver resolver(io_ctx_);
        auto endpoints = resolver.resolve(config_.gateway_host, std::to_string(config_.gateway_tcp_port));
        boost::asio::connect(sock, endpoints);
        return sock;
    }

    /// WireHeader를 raw bytes로 직렬화 (big-endian)
    static std::array<uint8_t, WireHeader::SIZE> make_raw_header(uint32_t msg_id, uint32_t body_size, uint8_t flags = 0)
    {
        std::array<uint8_t, WireHeader::SIZE> buf{};
        buf[0] = WireHeader::CURRENT_VERSION;
        buf[1] = flags;
        // msg_id big-endian [2..5]
        buf[2] = static_cast<uint8_t>((msg_id >> 24) & 0xFF);
        buf[3] = static_cast<uint8_t>((msg_id >> 16) & 0xFF);
        buf[4] = static_cast<uint8_t>((msg_id >> 8) & 0xFF);
        buf[5] = static_cast<uint8_t>(msg_id & 0xFF);
        // body_size big-endian [6..9]
        buf[6] = static_cast<uint8_t>((body_size >> 24) & 0xFF);
        buf[7] = static_cast<uint8_t>((body_size >> 16) & 0xFF);
        buf[8] = static_cast<uint8_t>((body_size >> 8) & 0xFF);
        buf[9] = static_cast<uint8_t>(body_size & 0xFF);
        // reserved=0 [10..11] (zero-initialized)
        return buf;
    }
};

/// Protocol 1: WireHeader만 전송하고 body는 보내지 않음 (불완전 프레임)
///
/// body_size=64인 WireHeader만 전송 후 body를 보내지 않은 채 잠시 대기했다가
/// 연결 close. Gateway의 read_loop가 partial read를 올바르게 처리하고
/// 크래시 없이 계속 동작하는지 확인.
TEST_F(E2EStressProtocolFixture, IncompleteFrame)
{
    constexpr int kIncompleteCount = 5;

    for (int i = 0; i < kIncompleteCount; ++i)
    {
        SCOPED_TRACE("incomplete-frame iteration " + std::to_string(i));

        auto sock = connect_raw();

        // body_size=64이지만 body는 전송하지 않음 — 불완전 프레임
        auto hdr_buf = make_raw_header(1000 /* LoginRequest */, 64 /* body_size, but no body follows */);
        boost::system::error_code ec;
        boost::asio::write(sock, boost::asio::buffer(hdr_buf), ec);
        // 전송 오류는 무시 — 연결이 이미 끊겼을 수 있음

        // 잠시 대기: Gateway read_loop가 body 수신을 기다리는 동안 연결 끊김
        std::this_thread::sleep_for(std::chrono::milliseconds{100});

        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        sock.close(ec);
    }

    // Gateway가 partial read 처리를 완료할 시간 확보
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    // Gateway still alive: 새 클라이언트로 접속 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after incomplete frames";
    EXPECT_TRUE(probe.is_connected());
    probe.close();
}

/// Protocol 2: 65536(64KB) body_size 메시지 전송 (비정상적으로 큰 페이로드)
///
/// body_size=65536인 WireHeader + 해당 크기의 payload를 전송.
/// Gateway가 크래시 없이 에러 응답을 반환하거나 연결을 끊는지 확인.
/// WireHeader::MAX_BODY_SIZE(16MB)보다 작으므로 파싱은 통과되나
/// 실제 핸들러에서 거부될 수 있음.
TEST_F(E2EStressProtocolFixture, MaxSizeMessage)
{
    constexpr uint32_t kLargeBodySize = 65536; // 64KB

    auto sock = connect_raw();

    // 64KB body를 가진 WireHeader 전송
    auto hdr_buf = make_raw_header(1000 /* LoginRequest */, kLargeBodySize);
    boost::system::error_code ec;
    boost::asio::write(sock, boost::asio::buffer(hdr_buf), ec);

    if (!ec)
    {
        // 64KB payload 전송 (0으로 채움 — 실제 FlatBuffers 페이로드 아님)
        std::vector<uint8_t> large_payload(kLargeBodySize, 0x00);
        boost::asio::write(sock, boost::asio::buffer(large_payload), ec);
    }

    // 에러 응답이 오거나 연결이 끊기는지 확인 (크래시 아님)
    bool got_error_or_disconnect = false;
    if (!ec)
    {
        // 응답 헤더 수신 시도
        std::array<uint8_t, WireHeader::SIZE> resp_hdr_buf{};
        boost::asio::read(sock, boost::asio::buffer(resp_hdr_buf), ec);

        if (!ec)
        {
            // 응답이 왔다면 에러 플래그가 있어야 함 (invalid payload)
            auto hdr_result = WireHeader::parse(std::span<const uint8_t>(resp_hdr_buf.data(), resp_hdr_buf.size()));
            if (hdr_result.has_value())
            {
                std::cerr << "[E2E-DEBUG] MaxSizeMessage resp: msg_id=" << hdr_result->msg_id
                          << " flags=" << static_cast<int>(hdr_result->flags) << "\n";
                EXPECT_TRUE(hdr_result->flags & ERROR_RESPONSE)
                    << "Response to oversized payload should have ERROR_RESPONSE flag";
                got_error_or_disconnect = true;
            }
        }
        else
        {
            // 연결이 끊겼거나 타임아웃 — 정상 거부로 간주
            got_error_or_disconnect = true;
        }
    }
    else
    {
        // 전송 자체가 실패 — 서버가 연결을 끊음
        got_error_or_disconnect = true;
    }
    EXPECT_TRUE(got_error_or_disconnect)
        << "Server should either send error response or disconnect for oversized payload";

    sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    sock.close(ec);

    // Gateway still alive: 새 클라이언트로 접속 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after max-size message";
    EXPECT_TRUE(probe.is_connected());
    probe.close();
}

/// Protocol 3: 미등록 msg_id(0xFFFF)를 stress_messages_회 연속 전송 (InvalidMsgIdFlood)
///
/// 등록되지 않은 msg_id(0xFFFF)로 다수 요청을 연속 전송.
/// 각 요청에 대해 에러 응답을 받거나 연결이 유지되어야 함.
/// Gateway가 미등록 msg_id를 처리하면서 크래시하지 않는지 확인.
TEST_F(E2EStressProtocolFixture, InvalidMsgIdFlood)
{
    TcpClient client(io_ctx_, config_);
    client.connect();

    // 로그인 없이 미등록 msg_id를 직접 전송 (인증 불필요 경로에서 검증)
    // 미등록 msg_id로 빈 페이로드 전송
    int error_count = 0;
    const int flood_count = std::min(stress_messages_, 20); // 과도한 부하 방지

    for (int i = 0; i < flood_count; ++i)
    {
        SCOPED_TRACE("invalid-msg-id flood iteration " + std::to_string(i));

        // msg_id=0xFFFF (미등록): 빈 payload 전송
        try
        {
            client.send(0xFFFF, nullptr, 0);

            auto resp = client.recv(std::chrono::seconds{3});
            // 에러 응답(ERROR_RESPONSE 플래그) 또는 어떤 응답이든 수신되면 정상
            if (resp.flags & ERROR_RESPONSE)
            {
                error_count++;
            }
        }
        catch (const std::exception& ex)
        {
            // 연결이 끊겼거나 타임아웃 — 정상 거부로 간주
            std::cerr << "[E2E-DEBUG] InvalidMsgIdFlood iter=" << i << " exception: " << ex.what() << "\n";
            error_count++;
            break;
        }
    }

    // 에러 응답이 하나 이상 있어야 함 (미등록 msg_id이므로)
    EXPECT_GT(error_count, 0) << "Expected at least one error response for invalid msg_id flood";

    // 연결이 끊겼을 수 있으므로 새 클라이언트로 Gateway 생존 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after invalid msg_id flood";
    EXPECT_TRUE(probe.is_connected());
    probe.close();
}

} // namespace apex::e2e
