#include "e2e_test_fixture.hpp"

#include <flatbuffers/flatbuffers.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <WinSock2.h>
#else
#include <sys/socket.h>
#endif

namespace apex::e2e {

E2EConfig E2ETestFixture::config_{};

// ---------------------------------------------------------------------------
// Suite lifecycle
// ---------------------------------------------------------------------------

void E2ETestFixture::SetUpTestSuite() {
    // 1. Start infrastructure via docker-compose
    std::cout << "[E2E] Starting infrastructure...\n";
    int rc = std::system(
        "docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d "
        "--wait --timeout 60");
    ASSERT_EQ(rc, 0) << "docker-compose failed to start";

    // 2. Create Kafka topics (auto-create is enabled, but explicit is safer)
    std::cout << "[E2E] Creating Kafka topics...\n";
    rc = std::system(
        "docker compose -f apex_infra/docker/docker-compose.e2e.yml "
        "exec -T kafka bash /init-kafka.sh");
    if (rc != 0) {
        std::cerr << "[E2E] Warning: Kafka topic creation returned non-zero "
                  << "(auto-create may handle it)\n";
    }

    // 3. Start Gateway, Auth Service, Chat Service processes
    //    In a real E2E environment these would be started as background processes.
    //    For now, we assume they are launched externally or via a helper script.
    std::cout << "[E2E] Starting Gateway and services...\n";
    // TODO(e2e): Launch gateway.exe, auth_svc.exe, chat_svc.exe as child processes
    //   Options: std::system("start /B ...") on Windows, or boost::process

    // 4. Wait for Gateway to accept connections
    std::cout << "[E2E] Waiting for services to be ready...\n";
    auto deadline = std::chrono::steady_clock::now() + config_.startup_timeout;
    bool ready = false;

    while (std::chrono::steady_clock::now() < deadline) {
        try {
            boost::asio::io_context probe_ctx;
            boost::asio::ip::tcp::socket probe_sock(probe_ctx);
            boost::asio::ip::tcp::endpoint ep(
                boost::asio::ip::make_address(config_.gateway_host),
                config_.gateway_ws_port);
            probe_sock.connect(ep);
            probe_sock.close();
            ready = true;
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }

    if (!ready) {
        std::cerr << "[E2E] Warning: Could not connect to Gateway at "
                  << config_.gateway_host << ":" << config_.gateway_ws_port
                  << " within timeout. Tests may fail.\n";
    }

    std::cout << "[E2E] Infrastructure ready.\n";
}

void E2ETestFixture::TearDownTestSuite() {
    // 1. Terminate service processes
    std::cout << "[E2E] Stopping services...\n";
    // TODO(e2e): Send SIGTERM / TerminateProcess to child processes

    // 2. Tear down docker-compose
    std::cout << "[E2E] Stopping infrastructure...\n";
    std::system(
        "docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v");
}

void E2ETestFixture::SetUp() {
    // Per-test initialization (e.g., flush Redis test data)
}

void E2ETestFixture::TearDown() {
    // Per-test cleanup
}

// ---------------------------------------------------------------------------
// TcpClient
// ---------------------------------------------------------------------------

E2ETestFixture::TcpClient::TcpClient(boost::asio::io_context& io_ctx,
                                       const E2EConfig& config)
    : io_ctx_(io_ctx)
    , config_(config)
    , socket_(io_ctx)
{
}

E2ETestFixture::TcpClient::~TcpClient() {
    if (connected_) {
        try { close(); } catch (...) {}
    }
}

void E2ETestFixture::TcpClient::connect() {
    boost::asio::ip::tcp::resolver resolver(io_ctx_);
    auto endpoints = resolver.resolve(
        config_.gateway_host,
        std::to_string(config_.gateway_ws_port));
    boost::asio::connect(socket_, endpoints);
    connected_ = true;
}

void E2ETestFixture::TcpClient::send(uint32_t msg_id,
                                       const uint8_t* payload,
                                       size_t size)
{
    if (!connected_) {
        throw std::runtime_error("TcpClient::send() called on disconnected socket");
    }

    // Build WireHeader v2
    WireHeader hdr{};
    hdr.version = WireHeader::CURRENT_VERSION;
    hdr.flags = 0;
    hdr.msg_id = msg_id;
    hdr.body_size = static_cast<uint32_t>(size);

    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    hdr.serialize(std::span<uint8_t, WireHeader::SIZE>(hdr_buf));

    // Send header + payload
    boost::asio::write(socket_, boost::asio::buffer(hdr_buf));
    if (size > 0 && payload != nullptr) {
        boost::asio::write(socket_, boost::asio::buffer(payload, size));
    }
}

E2ETestFixture::TcpClient::Response
E2ETestFixture::TcpClient::recv(std::chrono::seconds timeout)
{
    if (!connected_) {
        throw std::runtime_error("TcpClient::recv() called on disconnected socket");
    }

    // Set socket receive timeout
#ifdef _WIN32
    DWORD timeout_ms = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count());
    tv.tv_usec = 0;
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 &tv, sizeof(tv));
#endif

    // Read header (12 bytes)
    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    boost::asio::read(socket_, boost::asio::buffer(hdr_buf));

    auto hdr_result = WireHeader::parse(
        std::span<const uint8_t>(hdr_buf.data(), hdr_buf.size()));
    if (!hdr_result.has_value()) {
        throw std::runtime_error("Failed to parse WireHeader in response");
    }
    auto& hdr = hdr_result.value();

    // Read body
    Response resp;
    resp.msg_id = hdr.msg_id;
    resp.flags = hdr.flags;
    if (hdr.body_size > 0) {
        resp.payload.resize(hdr.body_size);
        boost::asio::read(socket_,
                          boost::asio::buffer(resp.payload.data(),
                                              resp.payload.size()));
    }

    return resp;
}

void E2ETestFixture::TcpClient::close() {
    if (connected_) {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        connected_ = false;
    }
}

// ---------------------------------------------------------------------------
// Auth helpers
// ---------------------------------------------------------------------------

E2ETestFixture::AuthResult
E2ETestFixture::login(TcpClient& client,
                       const std::string& email,
                       const std::string& password)
{
    // Build LoginRequest FlatBuffers payload
    // Schema (apex_services/auth-svc/schemas/login_request.fbs):
    //   table LoginRequest { email: string (required); password: string (required); }
    // msg_id = 1000 (LoginRequest from msg_registry)
    //
    // We use raw FlatBuffers Table API to avoid cross-service generated header dependency.
    flatbuffers::FlatBufferBuilder fbb(256);

    auto email_off = fbb.CreateString(email);
    auto pw_off = fbb.CreateString(password);

    // Manually construct LoginRequest table
    // vtable layout: field 0 = email (offset 4), field 1 = password (offset 6)
    auto start = fbb.StartTable();
    fbb.AddOffset(4, email_off);
    fbb.AddOffset(6, pw_off);
    auto loc = fbb.EndTable(start);
    fbb.Finish(flatbuffers::Offset<void>(loc));

    client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());

    // Receive LoginResponse (msg_id = 1001)
    auto resp = client.recv(std::chrono::seconds{
        static_cast<long>(config_.request_timeout.count())});

    AuthResult result{};
    if (resp.msg_id == 1001 && !resp.payload.empty()) {
        // Parse LoginResponse using raw Table API:
        //   error: LoginError (uint16, field 0, vtable offset 4)
        //   access_token: string (field 1, vtable offset 6)
        //   refresh_token: string (field 2, vtable offset 8)
        //   user_id: uint64 (field 3, vtable offset 10)
        //   expires_in_sec: uint32 (field 4, vtable offset 12)
        auto* root = flatbuffers::GetRoot<flatbuffers::Table>(resp.payload.data());
        if (root) {
            auto* at = root->GetPointer<const flatbuffers::String*>(6);
            if (at) result.access_token = at->str();

            auto* rt = root->GetPointer<const flatbuffers::String*>(8);
            if (rt) result.refresh_token = rt->str();

            result.user_id = root->GetField<uint64_t>(10, 0);
            result.expires_in_sec = root->GetField<uint32_t>(12, 0);
        }
    }

    return result;
}

void E2ETestFixture::authenticate(TcpClient& client,
                                    const std::string& token)
{
    // Send JWT token binding to Gateway.
    // Gateway binds the JWT to the current session for subsequent request authorization.
    // System msg_id 3 is reserved for AuthenticateSession (convention).
    //
    // Payload: simple table with token string field.
    flatbuffers::FlatBufferBuilder fbb(static_cast<size_t>(256) + token.size());
    auto token_off = fbb.CreateString(token);
    auto start = fbb.StartTable();
    fbb.AddOffset(4, token_off);  // field 0: token string
    auto loc = fbb.EndTable(start);
    fbb.Finish(flatbuffers::Offset<void>(loc));

    client.send(3 /* AuthenticateSession */, fbb.GetBufferPointer(), fbb.GetSize());

    // Wait for acknowledgment (Gateway may or may not send explicit ack)
    try {
        auto resp = client.recv(std::chrono::seconds{3});
        (void)resp;  // Ignore -- binding is best-effort in E2E
    } catch (...) {
        // Timeout is acceptable if Gateway doesn't send explicit ack
    }
}

} // namespace apex::e2e
