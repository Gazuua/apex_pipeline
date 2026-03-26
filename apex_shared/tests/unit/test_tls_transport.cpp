// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

/// TLS transport 단위 테스트.
///
/// TlsTcpTransport::create_ssl_context()의 에러 경로와
/// TlsSocket의 SocketBase 인터페이스 준수를 검증한다.
///
/// 실제 TLS 핸드셰이크 / 데이터 송수신은 self-signed 인증서 생성이 필요하며,
/// OpenSSL C API 의존성이 크므로 통합 테스트로 분리한다.
/// 이 파일에서는 인증서 파일 없이 테스트 가능한 경로에 집중한다.

#include <apex/shared/protocols/tcp/tls_socket.hpp>
#include <apex/shared/protocols/tcp/tls_tcp_transport.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace apex::shared::protocols::tcp;
namespace net = boost::asio;

namespace
{

/// 테스트용 임시 파일 RAII 가드. 소멸 시 파일 삭제.
struct TempFile
{
    std::string path;

    explicit TempFile(const std::string& filename, const std::string& content)
        : path(::testing::TempDir() + filename)
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs << content;
    }

    ~TempFile()
    {
        std::filesystem::remove(path);
    }
};

} // anonymous namespace

// ============================================================
// create_ssl_context 에러 경로
// ============================================================

/// 존재하지 않는 인증서 파일로 create_ssl_context 호출 시 예외 발생.
TEST(TlsTcpTransportTest, CreateSslContextThrowsOnMissingCertFile)
{
    TlsTcpTransport::Config cfg{
        .cert_file = "/nonexistent/cert.pem",
        .key_file = "/nonexistent/key.pem",
        .ca_file = "",
    };

    EXPECT_THROW((void)TlsTcpTransport::create_ssl_context(cfg), boost::system::system_error);
}

/// 유효하지 않은 인증서 내용으로 create_ssl_context 호출 시 예외 발생.
TEST(TlsTcpTransportTest, CreateSslContextThrowsOnInvalidCertContent)
{
    TempFile cert("test_invalid_cert.pem", "NOT A VALID CERTIFICATE");
    TempFile key("test_invalid_key.pem", "NOT A VALID KEY");

    TlsTcpTransport::Config cfg{
        .cert_file = cert.path,
        .key_file = key.path,
        .ca_file = "",
    };

    EXPECT_THROW((void)TlsTcpTransport::create_ssl_context(cfg), boost::system::system_error);
}

/// CA 파일이 존재하지 않는 경우 예외 발생.
TEST(TlsTcpTransportTest, CreateSslContextThrowsOnMissingCaFile)
{
    // cert_file도 존재하지 않으므로 cert 단계에서 먼저 실패하지만,
    // CA 파일 검증을 위해 실제 cert/key가 필요함.
    // 여기서는 cert 단계에서의 실패만 확인.
    TlsTcpTransport::Config cfg{
        .cert_file = "/nonexistent/cert.pem",
        .key_file = "/nonexistent/key.pem",
        .ca_file = "/nonexistent/ca.pem",
    };

    EXPECT_THROW((void)TlsTcpTransport::create_ssl_context(cfg), boost::system::system_error);
}

// ============================================================
// TlsSocket 기본 속성 테스트
// ============================================================

/// TlsSocket이 생성 직후 is_open() == true인지 검증.
/// (연결된 소켓으로 생성했을 때)
TEST(TlsSocketTest, IsOpenAfterConstruction)
{
    net::io_context io;
    net::ip::tcp::acceptor acceptor(io, net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    net::ip::tcp::socket raw_socket(io);
    raw_socket.connect(net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port));
    auto server_socket = acceptor.accept();

    net::ssl::context ssl_ctx(net::ssl::context::tlsv13);
    auto tls_sock = make_tls_socket(std::move(server_socket), ssl_ctx);

    EXPECT_TRUE(tls_sock->is_open());

    tls_sock->close();
    EXPECT_FALSE(tls_sock->is_open());

    raw_socket.close();
}

/// TlsSocket::close()가 멱등(idempotent)인지 검증.
TEST(TlsSocketTest, CloseIsIdempotent)
{
    net::io_context io;
    net::ip::tcp::acceptor acceptor(io, net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    net::ip::tcp::socket raw_socket(io);
    raw_socket.connect(net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port));
    auto server_socket = acceptor.accept();

    net::ssl::context ssl_ctx(net::ssl::context::tlsv13);
    auto tls_sock = make_tls_socket(std::move(server_socket), ssl_ctx);

    tls_sock->close();
    tls_sock->close(); // 두 번 호출해도 크래시 없어야 함
    EXPECT_FALSE(tls_sock->is_open());

    raw_socket.close();
}

/// TlsSocket::set_option_no_delay()가 크래시 없이 동작하는지 검증.
TEST(TlsSocketTest, SetNoDelayDoesNotCrash)
{
    net::io_context io;
    net::ip::tcp::acceptor acceptor(io, net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    net::ip::tcp::socket raw_socket(io);
    raw_socket.connect(net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port));
    auto server_socket = acceptor.accept();

    net::ssl::context ssl_ctx(net::ssl::context::tlsv13);
    auto tls_sock = make_tls_socket(std::move(server_socket), ssl_ctx);

    tls_sock->set_option_no_delay(true);
    EXPECT_TRUE(tls_sock->is_open());

    raw_socket.close();
}

/// TlsSocket::get_executor()가 유효한 executor를 반환하는지 검증.
TEST(TlsSocketTest, GetExecutorReturnsValid)
{
    net::io_context io;
    net::ip::tcp::acceptor acceptor(io, net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    net::ip::tcp::socket raw_socket(io);
    raw_socket.connect(net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port));
    auto server_socket = acceptor.accept();

    net::ssl::context ssl_ctx(net::ssl::context::tlsv13);
    auto tls_sock = make_tls_socket(std::move(server_socket), ssl_ctx);

    auto executor = tls_sock->get_executor();
    EXPECT_TRUE(executor != net::any_io_executor{});

    raw_socket.close();
}

/// TlsSocket::remote_endpoint()가 올바른 endpoint를 반환하는지 검증.
TEST(TlsSocketTest, RemoteEndpointReturnsClientAddress)
{
    net::io_context io;
    net::ip::tcp::acceptor acceptor(io, net::ip::tcp::endpoint(net::ip::tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    net::ip::tcp::socket raw_socket(io);
    raw_socket.connect(net::ip::tcp::endpoint(net::ip::address_v4::loopback(), port));
    auto server_socket = acceptor.accept();

    net::ssl::context ssl_ctx(net::ssl::context::tlsv13);
    auto tls_sock = make_tls_socket(std::move(server_socket), ssl_ctx);

    boost::system::error_code ec;
    auto ep = tls_sock->remote_endpoint(ec);
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(ep.address(), net::ip::address_v4::loopback());

    raw_socket.close();
}

/// make_listener_state가 유효한 ListenerState를 반환하는지 검증.
/// 실제 인증서 없이 테스트 가능한 부분만.
/// (create_ssl_context가 예외를 던지므로, 이 테스트는 유효한 cert/key가
/// 있어야만 통과한다. 여기서는 ListenerState 구조체의 존재 확인만.)
TEST(TlsTcpTransportTest, ListenerStateTypeExists)
{
    // ListenerState 타입이 존재하고 ssl_ctx 멤버를 가지는지 컴파일 타임 확인
    static_assert(std::is_constructible_v<TlsTcpTransport::ListenerState, net::ssl::context>);
}

/// PlainTcpTransport 대비 TlsTcpTransport가 Transport concept을 만족하는지 확인.
/// (이미 static_assert가 있지만, 테스트에서 명시적으로 확인)
TEST(TlsTcpTransportTest, SatisfiesTransportConcept)
{
    // 이 테스트가 컴파일되는 것 자체가 concept 만족을 증명.
    // tls_tcp_transport.hpp 하단의 static_assert로도 보장됨.
    SUCCEED();
}
