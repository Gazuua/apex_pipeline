#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <thread>
#include <vector>

using namespace apex::core;
using boost::asio::ip::tcp;

// --- Helper: build a raw frame (header + payload) ---

static std::vector<uint8_t> build_raw_frame(uint32_t msg_id, std::span<const uint8_t> payload, uint8_t flags = 0)
{
    WireHeader h{
        .flags = flags,
        .msg_id = msg_id,
        .body_size = static_cast<uint32_t>(payload.size()),
        .reserved = {},
    };
    auto hdr = h.serialize();
    std::vector<uint8_t> frame(hdr.begin(), hdr.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// --- Echo server: accept, read frames, echo back ---

static void run_echo_server(tcp::acceptor& acceptor)
{
    auto sock = acceptor.accept();
    RingBuffer recv_buf(4096);

    // Read until connection closes
    boost::system::error_code ec;
    std::array<uint8_t, 1024> tmp{};

    while (true)
    {
        auto n = sock.read_some(boost::asio::buffer(tmp), ec);
        if (ec || n == 0)
            break;

        // Copy received data into RingBuffer
        auto w = recv_buf.writable();
        size_t to_copy = std::min(w.size(), n);
        std::memcpy(w.data(), tmp.data(), to_copy);
        recv_buf.commit_write(to_copy);

        // Process all complete frames
        while (auto frame = FrameCodec::try_decode(recv_buf))
        {
            // Echo: send back the same frame
            std::vector<uint8_t> response(frame->header.frame_size());
            (void)FrameCodec::encode_to(response, frame->header, frame->payload);
            boost::asio::write(sock, boost::asio::buffer(response), ec);
            if (ec)
                return;

            FrameCodec::consume_frame(recv_buf, *frame);
        }
    }
}

// --- Tests ---

class EchoIntegrationTest : public ::testing::Test
{
  protected:
    boost::asio::io_context io_ctx_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    uint16_t port_{0};
    std::thread server_thread_;

    void SetUp() override
    {
        acceptor_ = std::make_unique<tcp::acceptor>(io_ctx_, tcp::endpoint(tcp::v4(), 0));
        port_ = acceptor_->local_endpoint().port();
    }

    void TearDown() override
    {
        if (server_thread_.joinable())
        {
            server_thread_.join();
        }
    }

    void start_server()
    {
        server_thread_ = std::thread([this] { run_echo_server(*acceptor_); });
    }

    tcp::socket connect_client()
    {
        tcp::socket client(io_ctx_);
        client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port_));
        return client;
    }
};

TEST_F(EchoIntegrationTest, SingleFrameRoundtrip)
{
    start_server();
    auto client = connect_client();

    // Send a frame
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    auto frame = build_raw_frame(0x0042, payload);
    boost::asio::write(client, boost::asio::buffer(frame));

    // Read response
    std::vector<uint8_t> response(frame.size());
    boost::asio::read(client, boost::asio::buffer(response));

    // Parse and verify
    auto header = WireHeader::parse(response);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->version, WireHeader::CURRENT_VERSION);
    EXPECT_EQ(header->msg_id, 0x0042);
    EXPECT_EQ(header->body_size, 4);

    // Verify payload
    std::span<const uint8_t> resp_payload(response.data() + WireHeader::SIZE, header->body_size);
    EXPECT_EQ(std::vector<uint8_t>(resp_payload.begin(), resp_payload.end()), payload);

    client.close();
}

TEST_F(EchoIntegrationTest, MultipleFrameRoundtrip)
{
    start_server();
    auto client = connect_client();

    struct TestCase
    {
        uint32_t msg_id;
        std::vector<uint8_t> payload;
    };

    std::vector<TestCase> cases = {
        {0x0001, {0x01}},
        {0x0002, {0x02, 0x03}},
        {0x0003, {0x04, 0x05, 0x06, 0x07}},
    };

    for (const auto& tc : cases)
    {
        auto frame = build_raw_frame(tc.msg_id, tc.payload);
        boost::asio::write(client, boost::asio::buffer(frame));

        std::vector<uint8_t> response(frame.size());
        boost::asio::read(client, boost::asio::buffer(response));

        auto header = WireHeader::parse(response);
        ASSERT_TRUE(header.has_value());
        EXPECT_EQ(header->msg_id, tc.msg_id);
        EXPECT_EQ(header->body_size, tc.payload.size());

        std::span<const uint8_t> resp_payload(response.data() + WireHeader::SIZE, header->body_size);
        EXPECT_EQ(std::vector<uint8_t>(resp_payload.begin(), resp_payload.end()), tc.payload);
    }

    client.close();
}

TEST_F(EchoIntegrationTest, ZeroPayloadFrame)
{
    start_server();
    auto client = connect_client();

    auto frame = build_raw_frame(0x00FF, {});
    boost::asio::write(client, boost::asio::buffer(frame));

    std::vector<uint8_t> response(frame.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto header = WireHeader::parse(response);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->msg_id, 0x00FF);
    EXPECT_EQ(header->body_size, 0);

    client.close();
}

TEST_F(EchoIntegrationTest, LargePayload)
{
    start_server();
    auto client = connect_client();

    // 1KB payload
    std::vector<uint8_t> payload(1024);
    for (size_t i = 0; i < payload.size(); ++i)
    {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto frame = build_raw_frame(0x0010, payload);
    boost::asio::write(client, boost::asio::buffer(frame));

    std::vector<uint8_t> response(frame.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto header = WireHeader::parse(response);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->body_size, 1024);

    std::span<const uint8_t> resp_payload(response.data() + WireHeader::SIZE, header->body_size);
    EXPECT_EQ(std::vector<uint8_t>(resp_payload.begin(), resp_payload.end()), payload);

    client.close();
}

// T6: Client abrupt disconnect — server should not hang
TEST_F(EchoIntegrationTest, AbruptClientDisconnect)
{
    start_server();

    {
        auto client = connect_client();
        // Send one frame
        std::vector<uint8_t> payload = {0x01, 0x02};
        auto frame = build_raw_frame(0x0001, payload);
        boost::asio::write(client, boost::asio::buffer(frame));

        // Read the echo back
        std::vector<uint8_t> response(frame.size());
        boost::asio::read(client, boost::asio::buffer(response));

        // Close abruptly (socket destructor)
    }

    // Server thread should exit cleanly (TearDown joins it)
    // If server hangs on read, the test will timeout
}

TEST_F(EchoIntegrationTest, FlagsPreserved)
{
    start_server();
    auto client = connect_client();

    std::vector<uint8_t> payload = {0x42};
    uint8_t flags = wire_flags::COMPRESSED | wire_flags::HEARTBEAT;
    auto frame = build_raw_frame(0x0001, payload, flags);
    boost::asio::write(client, boost::asio::buffer(frame));

    std::vector<uint8_t> response(frame.size());
    boost::asio::read(client, boost::asio::buffer(response));

    auto header = WireHeader::parse(response);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->flags, flags);

    client.close();
}
