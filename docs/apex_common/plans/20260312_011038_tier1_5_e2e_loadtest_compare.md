# Phase 5.5 Tier 1.5: E2E 부하 테스터 + Before/After 비교 도구 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tier 1 최적화의 실제 효과를 E2E 수준에서 검증하기 위한 부하 테스터(C++)와 JSON 결과 비교 스크립트(Python)를 구축한다.

**Architecture:** echo_loadtest는 Boost.Asio 기반 비동기 TCP 클라이언트 풀로 multicore_echo_server에 부하를 인가하고, throughput/latency 결과를 JSON으로 출력. compare_results.py는 두 JSON 파일을 읽어 stdout 비교 테이블을 생성.

**Tech Stack:** C++23, Boost.Asio, FlatBuffers, Python 3.10+

**v6 계획서 참조**: `docs/apex_common/plans/20260311_204613_phase5_5_v6.md` § 5.4, § 5.5, § 6 Tier 1.5

**선행 조건**: Tier 1 (핫패스 병목 제거 + Cross-Core 아키텍처 전환) 완료

---

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `apex_core/examples/echo_loadtest.cpp` | E2E echo 부하 테스터 (비동기 TCP 클라이언트 풀) |
| `apex_tools/benchmark/compare/compare_results.py` | JSON before/after 비교 스크립트 |

### Modified Files

| File | Change |
|------|--------|
| `apex_core/CMakeLists.txt` | `echo_loadtest` 실행 파일 빌드 추가 (examples 섹션) |

> **빌드 위치 결정**: loadtest는 apex_core의 wire protocol + FlatBuffers에 의존하므로 `apex_core/examples/`에 배치하고 apex_core CMakeLists.txt에서 빌드. `apex_tools/benchmark/loadtest/`에는 배치하지 않음 — 별도 CMakeLists.txt 없이 기존 빌드 시스템에 통합.

---

## Chunk 1: E2E 부하 테스터 구축 (Tasks 1–2)

### Task 1: echo_loadtest.cpp — 비동기 TCP 부하 테스터

**Files:**
- Create: `apex_core/examples/echo_loadtest.cpp`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1: CMakeLists.txt — echo_loadtest 빌드 추가**

`apex_core/CMakeLists.txt`의 examples 섹션에 추가:
```cmake
add_executable(echo_loadtest examples/echo_loadtest.cpp)
target_link_libraries(echo_loadtest PRIVATE apex_core)
```

> loadtest는 apex_core 라이브러리에 링크하여 WireHeader::serialize/parse + FlatBuffers 생성 코드를 재사용.

- [ ] **Step 2: echo_loadtest.cpp 작성**

```cpp
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
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
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
    std::string system_info;
};

// --- Build valid echo request frame ---
// Uses WireHeader::serialize() for correct 10B big-endian wire format
// and FlatBuffers EchoRequest for valid payload.
static std::vector<uint8_t> build_echo_request(uint32_t payload_size) {
    // Build FlatBuffers EchoRequest with 'data' field
    flatbuffers::FlatBufferBuilder fbb(payload_size + 64);
    std::vector<uint8_t> payload_data(payload_size, 0xAB);
    auto data_vec = fbb.CreateVector(payload_data);
    auto req = apex::messages::CreateEchoRequest(fbb, data_vec);
    fbb.Finish(req);

    // Serialize WireHeader (10B big-endian)
    WireHeader header{
        .msg_id = 0x0001,  // EchoRequest msg_id
        .body_size = static_cast<uint32_t>(fbb.GetSize())
    };
    auto hdr_bytes = header.serialize();  // std::array<uint8_t, 10>

    // Combine: header + FlatBuffers body
    std::vector<uint8_t> frame(hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(),
                 fbb.GetBufferPointer(),
                 fbb.GetBufferPointer() + fbb.GetSize());
    return frame;
}

// --- Echo Client Coroutine ---
net::awaitable<void> run_client(
    tcp::endpoint endpoint,
    const std::vector<uint8_t>& send_frame,  // pre-built frame (shared, read-only)
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

    // Response buffer — WireHeader::SIZE(10) + max body
    std::vector<uint8_t> recv_buf(WireHeader::SIZE + 64 * 1024);

    while (!stop_flag.load(std::memory_order_relaxed)) {
        auto t0 = steady_clock::now();

        // Send pre-built echo request frame
        try {
            co_await net::async_write(socket,
                net::buffer(send_frame), net::use_awaitable);
        } catch (...) {
            co_return;
        }

        // Read response header (10 bytes, big-endian)
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
            co_return;  // Invalid header
        }

        // Read response body
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

        if (measuring.load(std::memory_order_relaxed)) {
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
    // Manual JSON output — no external dependency needed.
    // system_info is simple key=value format, no escaping needed.
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

    // Pre-build echo request frame (shared across all connections, read-only)
    auto send_frame = build_echo_request(config.payload_size);

    // Allocate per-connection stats
    std::vector<ConnStats> all_stats(config.connections);
    std::atomic<bool> measuring{false};
    std::atomic<bool> stop_flag{false};

    if (!config.json_output) {
        std::cout << "Echo Load Test: " << config.connections << " connections, "
                  << config.payload_size << "B payload, "
                  << config.duration_secs << "s duration\n";
        std::cout << "Connecting to " << config.host << ":" << config.port << "...\n";
    }

    // Run clients on io_context pool (1 thread per logical core)
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

    result.msg_per_sec = static_cast<double>(result.total_received) / result.duration_secs;
    result.mb_per_sec = static_cast<double>(result.total_received)
        * (WireHeader::SIZE + config.payload_size) / (1024.0 * 1024.0) / result.duration_secs;

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
```

> **핵심 수정 사항 (리뷰 반영)**:
> 1. **WireHeader 10B big-endian**: `WireHeader::serialize()`/`WireHeader::parse()` 사용. 직접 struct 캐스팅 제거.
> 2. **FlatBuffers EchoRequest 페이로드**: `flatbuffers::FlatBufferBuilder`로 유효한 EchoRequest 빌드. 0xAB raw 데이터는 FlatBuffers 벡터 내부에 배치.
> 3. **apex_core 링크**: loadtest가 apex_core 라이브러리에 링크하여 WireHeader + FlatBuffers 생성 코드 재사용.
> 4. **system_info 인라인 출력**: 별도 변수 저장 없이 직접 std::cout에 출력하여 JSON 이스케이프 문제 회피.
> 5. **send_frame 사전 빌드**: 모든 커넥션이 동일한 프레임을 공유 (읽기 전용). 커넥션마다 재빌드 불필요.

- [ ] **Step 3: 빌드 확인**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug"`
Expected: `apex_core/bin/echo_loadtest_debug.exe` 빌드 성공

- [ ] **Step 4: 커밋**

```bash
git add apex_core/examples/echo_loadtest.cpp \
        apex_core/CMakeLists.txt
git commit -m "feat(tools): E2E echo 부하 테스터 (echo_loadtest)"
```

---

### Task 2: compare_results.py — JSON before/after 비교 스크립트

**Files:**
- Create: `apex_tools/benchmark/compare/compare_results.py`

- [ ] **Step 1: compare_results.py 작성**

```python
#!/usr/bin/env python3
"""
compare_results.py — JSON benchmark result comparison tool.

Usage:
    python compare_results.py <before.json> <after.json>

Output:
    Formatted comparison table to stdout showing deltas and percent changes.
"""

import json
import sys
from pathlib import Path


def load_json(path: str) -> dict:
    with open(path, "r") as f:
        return json.load(f)


def fmt_delta(before: float, after: float, lower_is_better: bool = True) -> str:
    """Format delta with ASCII indicator."""
    if before == 0:
        return f"{after:.1f} (new)"
    delta = after - before
    pct = (delta / before) * 100
    sign = "+" if delta > 0 else ""
    # For latency: lower is better. For throughput: higher is better.
    if lower_is_better:
        indicator = "[OK]" if delta < 0 else "[!!]" if delta > 0 else "[==]"
    else:
        indicator = "[OK]" if delta > 0 else "[!!]" if delta < 0 else "[==]"
    return f"{after:.1f} ({sign}{pct:.1f}%) {indicator}"


def compare(before: dict, after: dict) -> None:
    print("=" * 70)
    print(f"{'Metric':<25} {'Before':>15} {'After':>25}")
    print("=" * 70)

    # Throughput metrics (higher is better)
    for key, label in [
        ("msg_per_sec", "Throughput (msg/s)"),
        ("mb_per_sec", "Throughput (MB/s)"),
    ]:
        b = before.get(key, 0)
        a = after.get(key, 0)
        print(f"{label:<25} {b:>15.1f} {fmt_delta(b, a, lower_is_better=False):>25}")

    # Latency metrics (lower is better)
    b_lat = before.get("latency_us", {})
    a_lat = after.get("latency_us", {})
    for key, label in [
        ("avg", "Latency avg (us)"),
        ("p50", "Latency p50 (us)"),
        ("p90", "Latency p90 (us)"),
        ("p99", "Latency p99 (us)"),
        ("p999", "Latency p99.9 (us)"),
    ]:
        b = b_lat.get(key, 0)
        a = a_lat.get(key, 0)
        print(f"{label:<25} {b:>15.1f} {fmt_delta(b, a, lower_is_better=True):>25}")

    # Config
    print("-" * 70)
    print(f"{'Connections':<25} {before.get('connections', '?'):>15} {after.get('connections', '?'):>25}")
    print(f"{'Payload (bytes)':<25} {before.get('payload_size', '?'):>15} {after.get('payload_size', '?'):>25}")
    print(f"{'Duration (s)':<25} {before.get('duration_secs', '?'):>15} {after.get('duration_secs', '?'):>25}")
    print("=" * 70)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <before.json> <after.json>", file=sys.stderr)
        sys.exit(1)

    before_path, after_path = sys.argv[1], sys.argv[2]
    for p in [before_path, after_path]:
        if not Path(p).exists():
            print(f"Error: {p} not found", file=sys.stderr)
            sys.exit(1)

    before = load_json(before_path)
    after = load_json(after_path)
    compare(before, after)


if __name__ == "__main__":
    main()
```

> **리뷰 반영**: Unicode emoji (checkmark/cross) → ASCII `[OK]`/`[!!]`/`[==]` — Windows cp949 터미널 호환.

- [ ] **Step 2: 로컬 테스트 (더미 데이터)**

```bash
echo '{"msg_per_sec": 50000, "mb_per_sec": 12.5, "latency_us": {"avg": 200, "p50": 150, "p90": 300, "p99": 500, "p999": 1000}, "connections": 100, "payload_size": 256, "duration_secs": 30}' > /tmp/before.json
echo '{"msg_per_sec": 75000, "mb_per_sec": 18.7, "latency_us": {"avg": 130, "p50": 100, "p90": 200, "p99": 350, "p999": 700}, "connections": 100, "payload_size": 256, "duration_secs": 30}' > /tmp/after.json
python3 apex_tools/benchmark/compare/compare_results.py /tmp/before.json /tmp/after.json
```

Expected: 비교 테이블 출력, throughput [OK] (상승), latency [OK] (하강)

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/benchmark/compare/compare_results.py
git commit -m "feat(tools): JSON before/after 비교 스크립트 (compare_results.py)"
```

---

## Chunk 2: Tier 1 종합 Before/After 검증 (Task 3)

### Task 3: Tier 1 종합 before/after 수치 확인

**Files:**
- 실행만 (파일 생성 없음, 결과 JSON은 벤치마크 출력)

> 이 Task는 Tier 0 (벤치마크 인프라) + Tier 0.5 (에러 통일) + Tier 1 (핫패스 최적화)이 모두 구현된 상태에서 실행.

- [ ] **Step 1: baseline 측정 (Tier 0.5 완료 시점)**

```bash
# 현재 워크트리에서 Tier 0.5 완료 커밋으로 체크아웃
git stash  # 필요 시
git checkout <tier0.5-complete-commit-hash>

# 서버 빌드
cmd.exe //c "D:\.workspace\apex_core\build.bat debug"

# 서버 시작 (별도 터미널)
# Windows: start 명령으로 백그라운드 실행
start /B apex_core\bin\multicore_echo_server_debug.exe 9000 4

# baseline 측정 (10초 짧은 측정)
mkdir -p results
apex_core\bin\echo_loadtest_debug.exe --port=9000 --connections=200 --duration=10 --payload=256 --json > results/baseline_tier0.5.json

# 서버 종료 (Ctrl+C 또는 taskkill)
taskkill /IM multicore_echo_server_debug.exe /F
```

> 실제 실행 시 커밋 해시와 connection 수는 시스템 사양에 맞게 조절.
> Windows에서는 `&` 백그라운드 대신 `start /B`를 사용.

- [ ] **Step 2: after 측정 (Tier 1 완료 시점)**

```bash
# Tier 1 완료 커밋으로 체크아웃
git checkout <tier1-complete-commit-hash>

# 서버 빌드 + 시작
cmd.exe //c "D:\.workspace\apex_core\build.bat debug"
start /B apex_core\bin\multicore_echo_server_debug.exe 9000 4

# after 측정
apex_core\bin\echo_loadtest_debug.exe --port=9000 --connections=200 --duration=10 --payload=256 --json > results/after_tier1.json

# 서버 종료
taskkill /IM multicore_echo_server_debug.exe /F
```

- [ ] **Step 3: 비교**

```bash
python3 apex_tools/benchmark/compare/compare_results.py results/baseline_tier0.5.json results/after_tier1.json
```

Expected: Tier 1 최적화 효과 확인
- throughput 상승 (message passing + zero-copy)
- latency 하강 (이벤트 기반 drain + flat_map dispatch)

- [ ] **Step 4: 결과 JSON 커밋 (선택)**

```bash
git add results/baseline_tier0.5.json results/after_tier1.json
git commit -m "bench: Tier 1 before/after E2E 벤치마크 결과"
```

---

## Summary

### 핵심 산출물 2개
1. **echo_loadtest** (C++) — 비동기 TCP 부하 테스터, FlatBuffers EchoRequest + WireHeader 10B big-endian, `--json` 출력
2. **compare_results.py** (Python) — JSON 비교 테이블 생성 (ASCII 포맷, Windows cp949 호환)

### 디렉토리 구조
```
apex_core/examples/
    echo_loadtest.cpp         <- NEW (apex_core에서 빌드)

apex_tools/benchmark/
    compare/
        compare_results.py    <- NEW
```

### Task 별 커밋 (2-3개)
1. `feat(tools): E2E echo 부하 테스터 (echo_loadtest)`
2. `feat(tools): JSON before/after 비교 스크립트 (compare_results.py)`
3. `bench: Tier 1 before/after E2E 벤치마크 결과` (선택)
