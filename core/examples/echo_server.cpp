/// Apex Pipeline - Echo Server Example
///
/// Demonstrates core framework components working together:
/// - CoreEngine (io_context-per-core)
/// - ServiceBase<T> (CRTP service definition)
/// - WireHeader + FrameCodec (wire protocol framing)
/// - RingBuffer (receive buffering)
///
/// Usage: echo_server [port] [num_cores]
///   Default: port=9000, num_cores=2

#include <apex/core/core_engine.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/write.hpp>

#include <cstring>
#include <iostream>
#include <memory>

using namespace apex::core;
using boost::asio::ip::tcp;

// --- EchoService: handles echo requests ---

class EchoService : public ServiceBase<EchoService> {
public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override {
        handle(0x0001, &EchoService::on_echo);
        handle(0x0002, &EchoService::on_ping);
        std::cout << "[EchoService] Started. Handlers: echo(0x0001), ping(0x0002)\n";
    }

    void on_stop() override {
        std::cout << "[EchoService] Stopped. Total: " << echo_count_
                  << " echo, " << ping_count_ << " ping\n";
    }

    void on_echo(uint16_t, std::span<const uint8_t> payload) {
        ++echo_count_;
        // In a full implementation, we'd send back via session.
        // For this example, the session handles the echo directly.
    }

    void on_ping(uint16_t, std::span<const uint8_t>) {
        ++ping_count_;
    }

private:
    int echo_count_{0};
    int ping_count_{0};
};

// --- EchoSession: handles one TCP connection ---

class EchoSession : public std::enable_shared_from_this<EchoSession> {
public:
    EchoSession(tcp::socket socket, EchoService& service)
        : socket_(std::move(socket))
        , recv_buf_(8192)
        , service_(service)
    {
    }

    void start() {
        std::cout << "[Session] Client connected: "
                  << socket_.remote_endpoint() << "\n";
        do_read();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(tmp_buf_),
            [this, self](boost::system::error_code ec, size_t bytes) {
                if (ec) {
                    std::cout << "[Session] Client disconnected\n";
                    return;
                }

                // Copy to ring buffer (loop handles wrap-around)
                size_t copied = 0;
                while (copied < bytes) {
                    auto w = recv_buf_.writable();
                    if (w.empty()) {
                        // Buffer full — process frames to free space
                        process_frames();
                        w = recv_buf_.writable();
                        if (w.empty()) {
                            std::cerr << "[Session] Ring buffer full, dropping connection\n";
                            return;
                        }
                    }
                    size_t to_copy = std::min(w.size(), bytes - copied);
                    std::memcpy(w.data(), tmp_buf_.data() + copied, to_copy);
                    recv_buf_.commit_write(to_copy);
                    copied += to_copy;
                }

                // Process frames
                process_frames();

                // Continue reading
                do_read();
            });
    }

    void process_frames() {
        while (auto frame = FrameCodec::try_decode(recv_buf_)) {
            // Dispatch to service (for counting/logging)
            (void)service_.dispatcher().dispatch(
                frame->header.msg_id, frame->payload);

            // Echo: send back the same frame
            std::vector<uint8_t> response(frame->header.frame_size());
            (void)FrameCodec::encode_to(response, frame->header, frame->payload);

            boost::system::error_code ec;
            boost::asio::write(socket_, boost::asio::buffer(response), ec);
            if (ec) return;

            FrameCodec::consume_frame(recv_buf_, *frame);
        }
    }

    tcp::socket socket_;
    RingBuffer recv_buf_;
    EchoService& service_;
    std::array<uint8_t, 4096> tmp_buf_{};
};

// --- Main ---

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    uint32_t num_cores = 2;

    if (argc >= 2) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3) num_cores = static_cast<uint32_t>(std::atoi(argv[2]));

    std::cout << "=== Apex Pipeline Echo Server ===\n"
              << "Port: " << port << ", Cores: " << num_cores << "\n\n";

    // Create and start service
    EchoService service;
    service.start();

    // For this example, we use a simple single-threaded acceptor.
    // In production, CoreEngine would manage per-core io_contexts
    // with SO_REUSEPORT (Linux) or round-robin assignment.
    boost::asio::io_context io_ctx;

    // TCP acceptor
    tcp::acceptor acceptor(io_ctx, tcp::endpoint(tcp::v4(), port));
    std::cout << "[Server] Listening on port " << port << "\n";

    // Accept loop
    std::function<void()> do_accept;
    do_accept = [&] {
        acceptor.async_accept([&](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<EchoSession>(
                    std::move(socket), service);
                session->start();
            }
            do_accept();
        });
    };
    do_accept();

    // Graceful shutdown on SIGINT/SIGTERM
    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) {
        std::cout << "\n[Server] Shutting down...\n";
        acceptor.close();
        io_ctx.stop();
    });

    io_ctx.run();

    service.stop();
    std::cout << "[Server] Done.\n";
    return 0;
}
