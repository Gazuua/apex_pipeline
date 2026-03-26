// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>

#include "../test_helpers.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace apex::core;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

/// 별도 io_context에서 raw TCP 소켓으로 HTTP 요청을 보내고 전체 응답 문자열을 반환.
std::string send_http_request(uint16_t port, const std::string& method, const std::string& path)
{
    net::io_context client_io;
    tcp::socket sock(client_io);
    sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port));

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    net::write(sock, net::buffer(request));

    // 서버가 shutdown_send를 하므로 EOF까지 읽음
    std::string response;
    boost::system::error_code ec;
    for (;;)
    {
        char buf[1024];
        auto n = sock.read_some(net::buffer(buf), ec);
        if (n > 0)
            response.append(buf, n);
        if (ec)
            break;
    }
    return response;
}

/// HTTP 응답 문자열에서 상태 코드(숫자)를 추출. 파싱 실패 시 0 반환.
int extract_status_code(const std::string& response)
{
    // "HTTP/1.1 200 OK\r\n..." 형태에서 200 추출
    auto pos = response.find(' ');
    if (pos == std::string::npos)
        return 0;
    try
    {
        return std::stoi(response.substr(pos + 1));
    }
    catch (...)
    {
        return 0;
    }
}

/// HTTP 응답에서 body 부분을 추출 (\r\n\r\n 이후).
std::string extract_body(const std::string& response)
{
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos)
        return {};
    return response.substr(pos + 4);
}

/// HTTP 응답에서 특정 헤더 값을 추출 (대소문자 무시하지 않음).
std::string extract_header(const std::string& response, const std::string& header_name)
{
    std::string search = header_name + ": ";
    auto pos = response.find(search);
    if (pos == std::string::npos)
        return {};
    auto start = pos + search.size();
    auto end = response.find("\r\n", start);
    if (end == std::string::npos)
        return response.substr(start);
    return response.substr(start, end - start);
}

class MetricsHttpServerTest : public ::testing::Test
{
  protected:
    net::io_context io_ctx_;
    MetricsRegistry registry_;
    std::atomic<bool> running_{true};
    MetricsHttpServer server_;
    uint16_t port_{0};
    std::jthread server_thread_;

    void SetUp() override
    {
        server_.start(io_ctx_, 0, registry_, running_);
        port_ = server_.local_port();
        ASSERT_NE(port_, 0);
        server_thread_ = std::jthread([this] { io_ctx_.run(); });
    }

    void TearDown() override
    {
        server_.stop();
        io_ctx_.stop();
    }
};

} // anonymous namespace

TEST_F(MetricsHttpServerTest, MetricsEndpointReturnsPrometheusFormat)
{
    // 레지스트리에 카운터를 하나 등록해 serialize 결과가 비지 않도록
    auto& c = registry_.counter("test_total", "A test counter");
    c.increment(7);

    auto response = send_http_request(port_, "GET", "/metrics");
    EXPECT_EQ(extract_status_code(response), 200);

    auto content_type = extract_header(response, "Content-Type");
    EXPECT_NE(content_type.find("text/plain"), std::string::npos);

    auto body = extract_body(response);
    EXPECT_NE(body.find("test_total 7"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, HealthEndpointReturnsOk)
{
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("OK"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, ReadyEndpointReflectsRunningFlag)
{
    // running_ = true (SetUp 기본값) → 200
    auto response = send_http_request(port_, "GET", "/ready");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("READY"), std::string::npos);

    // running_ = false → 503
    running_.store(false, std::memory_order_release);
    response = send_http_request(port_, "GET", "/ready");
    EXPECT_EQ(extract_status_code(response), 503);

    body = extract_body(response);
    EXPECT_NE(body.find("NOT READY"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, UnknownPathReturns404)
{
    auto response = send_http_request(port_, "GET", "/invalid");
    EXPECT_EQ(extract_status_code(response), 404);

    auto body = extract_body(response);
    EXPECT_NE(body.find("Not Found"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, UnsupportedMethodReturns404)
{
    // 현재 서버는 메서드를 분기하지 않고 GET+path 매칭만 하므로 POST → else → 404
    auto response = send_http_request(port_, "POST", "/metrics");
    EXPECT_EQ(extract_status_code(response), 404);
}

TEST_F(MetricsHttpServerTest, MalformedRequest_EmptyPayload)
{
    // 빈 요청 전송 후 연결 종료 — 서버가 크래시하지 않아야 함
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));
        boost::system::error_code ec;
        sock.shutdown(tcp::socket::shutdown_send, ec);
        // 응답을 기대하지 않음 — 연결만 닫음
    }

    // 서버가 다음 정상 요청을 처리할 수 있는지 확인
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);
}

TEST_F(MetricsHttpServerTest, MalformedRequest_InvalidMethod)
{
    // 잘못된 HTTP 메서드
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));
        std::string raw = "INVALID /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        net::write(sock, net::buffer(raw));
        // 응답을 드레인
        boost::system::error_code ec;
        char buf[1024];
        while (!ec)
        {
            sock.read_some(net::buffer(buf), ec);
        }
    }

    // 서버가 다음 정상 요청을 처리할 수 있는지 확인
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);
}

TEST_F(MetricsHttpServerTest, MalformedRequest_IncompleteRequestLine)
{
    // 불완전한 요청라인
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));
        std::string raw = "GET\r\n\r\n";
        net::write(sock, net::buffer(raw));
        boost::system::error_code ec;
        char buf[1024];
        while (!ec)
        {
            sock.read_some(net::buffer(buf), ec);
        }
    }

    // 서버가 다음 정상 요청을 처리할 수 있는지 확인
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);
}

TEST_F(MetricsHttpServerTest, MalformedRequest_BinaryGarbage)
{
    // 이진 가비지 데이터
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));
        std::string garbage(64, '\xDE');
        garbage[10] = '\x00';
        garbage[20] = '\xFF';
        garbage[30] = '\x01';
        boost::system::error_code ec;
        net::write(sock, net::buffer(garbage), ec);
        sock.shutdown(tcp::socket::shutdown_send, ec);
        // 응답을 드레인
        char buf[1024];
        ec = {};
        while (!ec)
        {
            sock.read_some(net::buffer(buf), ec);
        }
    }

    // 서버가 다음 정상 요청을 처리할 수 있는지 확인
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);
}

TEST_F(MetricsHttpServerTest, ConcurrentConnections)
{
    constexpr int kThreads = 5;
    std::vector<int> results(kThreads, 0);
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([this, &results, i] {
            auto response = send_http_request(port_, "GET", "/health");
            results[static_cast<size_t>(i)] = extract_status_code(response);
        });
    }

    // jthread 소멸자가 join 수행
    threads.clear();

    for (int i = 0; i < kThreads; ++i)
    {
        EXPECT_EQ(results[static_cast<size_t>(i)], 200) << "Thread " << i << " failed";
    }
}

/// stop() 중 in-flight session이 cancel되어 깨끗하게 정리되는지 검증.
/// slow client(연결만 하고 요청 미전송)로 async_read 대기 상태를 만든 뒤
/// stop()으로 cancel_all()을 트리거하고, SessionGuard 소멸로 세션이
/// 정리되는지 확인한다.
TEST_F(MetricsHttpServerTest, InFlightSessionCancelledOnStop)
{
    // 1) slow client: 연결만 하고 HTTP 요청을 보내지 않음
    //    → 서버의 handle_session이 async_read에서 블로킹 대기
    net::io_context client_io;
    tcp::socket slow_sock(client_io);
    slow_sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));

    // 2) 서버가 accept → co_spawn → async_read 대기에 진입할 시간 확보.
    //    server_thread_가 io_ctx_.run()을 구동 중이므로 곧바로 처리됨.
    //    네트워크 왕복 + 코루틴 스케줄링 포함 여유 있는 대기.
    std::this_thread::sleep_for(std::chrono::milliseconds(50) * apex::test::timeout_multiplier());

    // 3) stop() 호출 → acceptor 닫기 + cancel_all() (소켓 cancel)
    //    cancel()은 async_read에 operation_aborted를 전달.
    //    server_thread_가 실행 중이므로 cancel 핸들러가 처리되어
    //    SessionGuard 소멸 → tracker에서 세션 제거.
    server_.stop();

    // 4) cancel 신호 처리를 위한 drain 대기.
    //    server_thread_가 io_ctx_.run()을 구동 중이므로 cancel 핸들러는
    //    해당 스레드에서 실행됨. 테스트 스레드에서는 충분한 시간만 보장.
    std::this_thread::sleep_for(std::chrono::milliseconds(100) * apex::test::timeout_multiplier());

    // 5) 검증: slow client 소켓에서 read 시도 → 서버가 세션을 cancel하여
    //    종료했으므로 EOF 또는 에러가 발생해야 함.
    boost::system::error_code ec;
    char buf[64];
    auto n = slow_sock.read_some(net::buffer(buf), ec);

    // 서버가 소켓을 cancel했으므로 두 가지 결과 중 하나:
    // - EOF (eof): 서버 측 소켓이 닫힌 경우
    // - connection_reset / connection_aborted: OS가 RST를 보낸 경우
    // 어떤 경우든 ec가 설정되거나 n==0이어야 하며, 정상 데이터(n>0 + !ec)는 불가.
    bool session_terminated = ec || n == 0;
    EXPECT_TRUE(session_terminated) << "Expected slow client to observe session termination, ec=" << ec.message();

    // 6) 소켓 정리
    if (slow_sock.is_open())
    {
        boost::system::error_code close_ec;
        slow_sock.close(close_ec);
    }

    // TearDown에서 server_.stop() 재호출은 안전 (acceptor 이미 닫힘).
    // io_ctx_.stop()으로 server_thread_ 종료.
}

/// 여러 slow client가 동시에 연결된 상태에서 stop() 호출 시
/// cancel_all()이 모든 in-flight session을 정리하는지 검증.
TEST_F(MetricsHttpServerTest, MultipleConcurrentSessionsCancelledOnStop)
{
    constexpr int kSlowClients = 5;
    std::vector<tcp::socket> slow_sockets;
    slow_sockets.reserve(kSlowClients);

    // 1) 여러 slow client: 연결만 하고 요청 미전송
    for (int i = 0; i < kSlowClients; ++i)
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));
        slow_sockets.push_back(std::move(sock));
    }

    // 2) 서버가 모든 연결을 accept하여 async_read 대기에 진입할 시간 확보
    std::this_thread::sleep_for(std::chrono::milliseconds(100) * apex::test::timeout_multiplier());

    // 3) stop() — 모든 세션 cancel
    server_.stop();

    // 4) cancel 처리 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(150) * apex::test::timeout_multiplier());

    // 5) 모든 slow client에서 EOF 또는 에러 확인
    for (int i = 0; i < kSlowClients; ++i)
    {
        boost::system::error_code ec;
        char buf[64];
        auto n = slow_sockets[static_cast<size_t>(i)].read_some(net::buffer(buf), ec);
        bool terminated = ec || n == 0;
        EXPECT_TRUE(terminated) << "Slow client " << i << " should observe termination, ec=" << ec.message();
    }

    // 6) 소켓 정리
    for (auto& sock : slow_sockets)
    {
        if (sock.is_open())
        {
            boost::system::error_code ec;
            sock.close(ec);
        }
    }
}

/// 큰 HTTP 헤더를 보내도 서버가 크래시하지 않고 다음 요청을 처리하는지 검증.
/// HttpServerBase는 Beast의 flat_buffer + empty_body로 파싱하므로
/// 과도하게 큰 헤더는 Beast가 거부하고 세션이 예외로 종료될 수 있다.
TEST_F(MetricsHttpServerTest, OversizedHeaderDoesNotCrashServer)
{
    // 1) 매우 긴 헤더를 포함한 요청 전송
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));

        // 64KB 크기의 헤더 값 생성
        std::string huge_header(64 * 1024, 'X');
        std::string request =
            "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Big: " + huge_header + "\r\nConnection: close\r\n\r\n";
        boost::system::error_code ec;
        net::write(sock, net::buffer(request), ec);
        // 응답 드레인 (에러 무시)
        char buf[1024];
        ec = {};
        while (!ec)
        {
            sock.read_some(net::buffer(buf), ec);
        }
    }

    // 2) 서버가 여전히 정상 요청을 처리할 수 있는지 확인
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);
}

/// 큰 HTTP body를 보내도 서버가 크래시하지 않고 다음 요청을 처리하는지 검증.
/// HttpServerBase의 run_session은 http::request<http::empty_body>를 사용하므로
/// body 데이터는 파싱되지 않지만, 소켓 버퍼에 남은 데이터가 문제를 일으키지 않아야 함.
TEST_F(MetricsHttpServerTest, LargeBodyDoesNotCrashServer)
{
    // 1) Content-Length가 큰 요청 전송
    {
        net::io_context client_io;
        tcp::socket sock(client_io);
        sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port_));

        std::string body(32 * 1024, 'A');
        std::string request =
            "POST /health HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        boost::system::error_code ec;
        net::write(sock, net::buffer(request), ec);
        // 응답 드레인
        char buf[1024];
        ec = {};
        while (!ec)
        {
            sock.read_some(net::buffer(buf), ec);
        }
    }

    // 2) 서버가 다음 요청을 처리할 수 있는지 확인
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);
}
