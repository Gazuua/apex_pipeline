/// echo_loadtest — E2E echo server load tester
///
/// Usage: echo_loadtest [options]
///   --host=HOST       Server host (default: 127.0.0.1)
///   --port=PORT       Server port (default: 9000)
///   --connections=N   Number of concurrent connections (default: auto = logical_cores * 50)
///   --duration=SECS   Test duration in seconds (default: 30)
///   --payload=BYTES   Echo payload size (default: 256)
///   --warmup=SECS     Warmup duration before measurement (default: 3)
///   --json            Output results as JSON to stdout

#include <apex/core/wire_header.hpp>

#include <generated/echo_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>

namespace net = boost::asio;
using tcp = net::ip::tcp;
using apex::core::WireHeader;
using namespace std::chrono;
using namespace std::chrono_literals;

// --- Config ---
struct LoadTestConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    uint32_t connections = 0;  // 0 = auto
    uint32_t duration_secs = 30;
    uint32_t payload_size = 256;
    uint32_t warmup_secs = 3;
    bool json_output = false;
};

// --- Per-Connection Stats ---
struct ConnStats {
    uint64_t messages_sent = 0;
    uint64_t messages_received = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    std::vector<uint64_t> latencies_us;  // per-message latency in microseconds
};

// --- Result ---
struct LoadTestResult {
    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    double msg_per_sec = 0;
    double mb_per_sec = 0;
    double p50_us = 0;
    double p90_us = 0;
    double p99_us = 0;
    double p999_us = 0;
    double avg_us = 0;
    uint32_t connections = 0;
    uint32_t payload_size = 0;
    double duration_secs = 0;
};

// --- Build valid echo request frame ---
static std::vector<uint8_t> build_echo_request(uint32_t payload_size) {
    flatbuffers::FlatBufferBuilder fbb(payload_size + 64);
    std::vector<uint8_t> payload_data(payload_size, 0xAB);
    auto data_vec = fbb.CreateVector(payload_data);
    auto req = apex::messages::CreateEchoRequest(fbb, data_vec);
    fbb.Finish(req);

    WireHeader header{
        .msg_id = 0x0001,  // EchoRequest msg_id
        .body_size = static_cast<uint32_t>(fbb.GetSize())
    };
    auto hdr_bytes = header.serialize();

    std::vector<uint8_t> frame(hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(),
                 fbb.GetBufferPointer(),
                 fbb.GetBufferPointer() + fbb.GetSize());
    return frame;
}

// --- Echo Client Coroutine ---
net::awaitable<void> run_client(
    tcp::endpoint endpoint,
    const std::vector<uint8_t>& send_frame,
    ConnStats& stats,
    std::atomic<bool>& measuring,
    std::atomic<bool>& stop_flag)
{
    auto executor = co_await net::this_coro::executor;
    tcp::socket socket(executor);

    try {
        co_await socket.async_connect(endpoint, net::use_awaitable);
    } catch (...) {
        co_return;
    }

    // Buffer sized to fit response: header + body (at least as large as request)
    std::vector<uint8_t> recv_buf(send_frame.size() + 256);

    while (!stop_flag.load(std::memory_order_acquire)) {
        auto t0 = steady_clock::now();

        try {
            co_await net::async_write(socket,
                net::buffer(send_frame), net::use_awaitable);
        } catch (...) {
            co_return;
        }

        try {
            co_await net::async_read(socket,
                net::buffer(recv_buf.data(), WireHeader::SIZE),
                net::use_awaitable);
        } catch (...) {
            co_return;
        }

        auto resp_hdr = WireHeader::parse(
            std::span<const uint8_t>(recv_buf.data(), WireHeader::SIZE));
        if (!resp_hdr) {
            co_return;
        }

        if (resp_hdr->body_size > recv_buf.size() - WireHeader::SIZE) {
            co_return;  // body too large for buffer
        }

        if (resp_hdr->body_size > 0) {
            try {
                co_await net::async_read(socket,
                    net::buffer(recv_buf.data() + WireHeader::SIZE, resp_hdr->body_size),
                    net::use_awaitable);
            } catch (...) {
                co_return;
            }
        }

        auto t1 = steady_clock::now();

        if (measuring.load(std::memory_order_acquire)) {
            ++stats.messages_sent;
            ++stats.messages_received;
            stats.bytes_sent += send_frame.size();
            stats.bytes_received += WireHeader::SIZE + resp_hdr->body_size;
            stats.latencies_us.push_back(
                static_cast<uint64_t>(duration_cast<microseconds>(t1 - t0).count()));
        }
    }

    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

LoadTestConfig parse_args(int argc, char* argv[]) {
    LoadTestConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        try {
            if (arg.starts_with("--host=")) config.host = arg.substr(7);
            else if (arg.starts_with("--port=")) config.port = static_cast<uint16_t>(std::stoi(arg.substr(7)));
            else if (arg.starts_with("--connections=")) {
                auto val = arg.substr(14);
                config.connections = (val == "auto") ? 0 : static_cast<uint32_t>(std::stoi(val));
            }
            else if (arg.starts_with("--duration=")) config.duration_secs = static_cast<uint32_t>(std::stoi(arg.substr(11)));
            else if (arg.starts_with("--payload=")) config.payload_size = static_cast<uint32_t>(std::stoi(arg.substr(10)));
            else if (arg.starts_with("--warmup=")) config.warmup_secs = static_cast<uint32_t>(std::stoi(arg.substr(9)));
            else if (arg == "--json") config.json_output = true;
        } catch (const std::exception& e) {
            std::cerr << "Invalid argument: " << arg << " (" << e.what() << ")\n";
            std::exit(1);
        }
    }
    if (config.connections == 0) {
        config.connections = std::max(1u, std::thread::hardware_concurrency() * 50);
    }
    return config;
}

void print_result_text(const LoadTestResult& r) {
    std::cout << "\n=== Echo Load Test Results ===\n";
    std::cout << "Connections: " << r.connections << "\n";
    std::cout << "Payload: " << r.payload_size << " bytes\n";
    std::cout << "Duration: " << r.duration_secs << "s\n";
    std::cout << "Messages: " << r.total_sent << " sent, " << r.total_received << " received\n";
    std::cout << "Throughput: " << r.msg_per_sec << " msg/s, " << r.mb_per_sec << " MB/s\n";
    std::cout << "Latency (us): avg=" << r.avg_us
              << " p50=" << r.p50_us
              << " p90=" << r.p90_us
              << " p99=" << r.p99_us
              << " p99.9=" << r.p999_us << "\n";
}

void print_result_json(const LoadTestResult& r) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\n";
    std::cout << "  \"connections\": " << r.connections << ",\n";
    std::cout << "  \"payload_size\": " << r.payload_size << ",\n";
    std::cout << "  \"duration_secs\": " << r.duration_secs << ",\n";
    std::cout << "  \"total_sent\": " << r.total_sent << ",\n";
    std::cout << "  \"total_received\": " << r.total_received << ",\n";
    std::cout << "  \"msg_per_sec\": " << r.msg_per_sec << ",\n";
    std::cout << "  \"mb_per_sec\": " << r.mb_per_sec << ",\n";
    std::cout << "  \"latency_us\": {\n";
    std::cout << "    \"avg\": " << r.avg_us << ",\n";
    std::cout << "    \"p50\": " << r.p50_us << ",\n";
    std::cout << "    \"p90\": " << r.p90_us << ",\n";
    std::cout << "    \"p99\": " << r.p99_us << ",\n";
    std::cout << "    \"p999\": " << r.p999_us << "\n";
    std::cout << "  },\n";
    std::cout << "  \"system_info\": \"cores=" << std::thread::hardware_concurrency()
              << " threads=" << std::max(1u, std::thread::hardware_concurrency()) << "\"\n";
    std::cout << "}\n";
}

int main(int argc, char* argv[]) {
    auto config = parse_args(argc, argv);

    auto resolver_io = net::io_context{};
    tcp::resolver resolver(resolver_io);
    auto endpoints = resolver.resolve(config.host, std::to_string(config.port));
    auto endpoint = *endpoints.begin();

    auto send_frame = build_echo_request(config.payload_size);

    std::vector<ConnStats> all_stats(config.connections);
    std::atomic<bool> measuring{false};
    std::atomic<bool> stop_flag{false};

    if (!config.json_output) {
        std::cout << "Echo Load Test: " << config.connections << " connections, "
                  << config.payload_size << "B payload, "
                  << config.duration_secs << "s duration\n";
        std::cout << "Connecting to " << config.host << ":" << config.port << "...\n";
    }

    uint32_t num_threads = std::max(1u, std::thread::hardware_concurrency());
    net::io_context io_ctx;

    for (uint32_t i = 0; i < config.connections; ++i) {
        net::co_spawn(io_ctx,
            run_client(endpoint, send_frame, all_stats[i], measuring, stop_flag),
            net::detached);
    }

    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&io_ctx] { io_ctx.run(); });
    }

    // Warmup phase
    if (!config.json_output) {
        std::cout << "Warmup: " << config.warmup_secs << "s...\n";
    }
    std::this_thread::sleep_for(seconds(config.warmup_secs));

    // Measurement phase
    measuring.store(true, std::memory_order_release);
    if (!config.json_output) {
        std::cout << "Measuring: " << config.duration_secs << "s...\n";
    }
    std::this_thread::sleep_for(seconds(config.duration_secs));
    measuring.store(false, std::memory_order_release);

    // Stop
    stop_flag.store(true, std::memory_order_release);
    io_ctx.stop();
    for (auto& t : threads) t.join();

    // Aggregate results
    LoadTestResult result;
    result.connections = config.connections;
    result.payload_size = config.payload_size;
    result.duration_secs = static_cast<double>(config.duration_secs);

    std::vector<uint64_t> all_latencies;
    for (auto& s : all_stats) {
        result.total_sent += s.messages_sent;
        result.total_received += s.messages_received;
        all_latencies.insert(all_latencies.end(),
            s.latencies_us.begin(), s.latencies_us.end());
    }

    uint64_t total_bytes = 0;
    for (auto& s : all_stats) {
        total_bytes += s.bytes_sent + s.bytes_received;
    }

    result.msg_per_sec = static_cast<double>(result.total_received) / result.duration_secs;
    result.mb_per_sec = static_cast<double>(total_bytes) / (1024.0 * 1024.0) / result.duration_secs;

    if (!all_latencies.empty()) {
        std::sort(all_latencies.begin(), all_latencies.end());
        auto n = all_latencies.size();
        auto percentile = [&](double p) {
            size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(n - 1));
            return static_cast<double>(all_latencies[std::min(idx, n - 1)]);
        };
        result.p50_us = percentile(50);
        result.p90_us = percentile(90);
        result.p99_us = percentile(99);
        result.p999_us = percentile(99.9);
        result.avg_us = static_cast<double>(
            std::accumulate(all_latencies.begin(), all_latencies.end(), uint64_t{0}))
            / static_cast<double>(n);
    }

    if (config.json_output) {
        print_result_json(result);
    } else {
        print_result_text(result);
    }

    return 0;
}
