# Phase 5.5 Tier 0: 벤치마크 인프라 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Google Benchmark 기반 마이크로/통합 벤치마크 11개 구축 + baseline JSON 측정

**Architecture:** `apex_core/benchmarks/` 하위에 `micro/` (7개)와 `integration/` (4개) 벤치마크를 배치. 공유 `bench_main.cpp`가 SystemProfile 자동 감지 + JSON context 주입을 담당. 각 벤치마크는 독립 실행 파일.

**Tech Stack:** Google Benchmark (vcpkg), CMake, Boost.Asio, C++23

---

## File Structure

### New Files

| File | Responsibility |
|------|---|
| `apex_core/benchmarks/CMakeLists.txt` | 벤치마크 빌드 설정 + `apex_add_benchmark()` 헬퍼 |
| `apex_core/benchmarks/system_profile.hpp` | 크로스플랫폼 시스템 사양 감지 (코어 수, RAM) |
| `apex_core/benchmarks/bench_main.cpp` | 커스텀 main: SystemProfile 출력 + JSON context 주입 |
| `apex_core/benchmarks/bench_helpers.hpp` | 소켓 쌍 생성, 프레임 빌드 등 공통 유틸 |
| `apex_core/benchmarks/micro/bench_mpsc_queue.cpp` | MPSC 1P1C/2P1C/3P1C + backpressure |
| `apex_core/benchmarks/micro/bench_ring_buffer.cpp` | write+read 사이클 (64B~4KB) + linearize |
| `apex_core/benchmarks/micro/bench_frame_codec.cpp` | encode/decode (64B~16KB) |
| `apex_core/benchmarks/micro/bench_dispatcher.cpp` | handler lookup (10/100/1000 등록) |
| `apex_core/benchmarks/micro/bench_timing_wheel.cpp` | schedule+tick (1K/10K/50K 엔트리) |
| `apex_core/benchmarks/micro/bench_slab_pool.cpp` | alloc/dealloc vs malloc vs make_shared |
| `apex_core/benchmarks/micro/bench_session_lifecycle.cpp` | Session 생성/shared_ptr 복사 비용 |
| `apex_core/benchmarks/integration/bench_cross_core_latency.cpp` | ping-pong RTT/2 |
| `apex_core/benchmarks/integration/bench_frame_pipeline.cpp` | decode→dispatch→encode |
| `apex_core/benchmarks/integration/bench_session_throughput.cpp` | TCP echo round-trip |
| `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp` | closure shipping baseline |

### Modified Files

| File | Change |
|------|---|
| `vcpkg.json` (루트) | `benchmark` 의존성 추가 |
| `apex_core/vcpkg.json` | `benchmark` 의존성 추가 |
| `apex_core/CMakeLists.txt` | `add_subdirectory(benchmarks)` 추가 |
| `CMakePresets.json` | `benchmark` configure/build preset 추가 |
| `.github/workflows/ci.yml` | 벤치마크 빌드 검증 (linux-gcc에서) |

---

## Chunk 1: 인프라 + 첫 벤치마크 (Tasks 1–3)

### Task 1: vcpkg + CMake 벤치마크 빌드 인프라

**Files:**
- Modify: `vcpkg.json` (루트)
- Modify: `apex_core/vcpkg.json`
- Create: `apex_core/benchmarks/CMakeLists.txt`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1: vcpkg.json에 benchmark 추가 (루트 + apex_core)**

`vcpkg.json` (루트):
```json
{
  "name": "apex-pipeline",
  "version-semver": "0.2.4",
  "builtin-baseline": "b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01",
  "dependencies": [
    { "name": "boost-asio", "version>=": "1.84.0" },
    "benchmark",
    "flatbuffers",
    "gtest",
    "spdlog",
    "tomlplusplus"
  ]
}
```

`apex_core/vcpkg.json`도 동일하게 `"benchmark"` 추가.

- [ ] **Step 2: benchmarks/CMakeLists.txt 생성**

```cmake
find_package(benchmark CONFIG QUIET)
if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found — benchmarks will not be built")
    return()
endif()

message(STATUS "Building benchmarks")

# Shared bench_main (custom main with SystemProfile)
add_library(apex_bench_main OBJECT bench_main.cpp)
target_include_directories(apex_bench_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(apex_bench_main PRIVATE benchmark::benchmark)

# Helper function — each benchmark is an independent executable
function(apex_add_benchmark BENCH_NAME BENCH_SOURCE)
    add_executable(${BENCH_NAME} ${BENCH_SOURCE} $<TARGET_OBJECTS:apex_bench_main>)
    target_include_directories(${BENCH_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${BENCH_NAME} PRIVATE apex::core benchmark::benchmark)
    if(WIN32)
        target_compile_options(${BENCH_NAME} PRIVATE /utf-8)
        target_compile_definitions(${BENCH_NAME} PRIVATE _WIN32_WINNT=0x0A00)
        target_link_libraries(${BENCH_NAME} PRIVATE ws2_32 mswsock)
    endif()
    # 빌드 출력을 apex_core/bin/ 에 통일
    set_target_properties(${BENCH_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
endfunction()

# --- Micro benchmarks ---
apex_add_benchmark(bench_mpsc_queue    micro/bench_mpsc_queue.cpp)
apex_add_benchmark(bench_ring_buffer   micro/bench_ring_buffer.cpp)
apex_add_benchmark(bench_frame_codec   micro/bench_frame_codec.cpp)
apex_add_benchmark(bench_dispatcher    micro/bench_dispatcher.cpp)
apex_add_benchmark(bench_timing_wheel  micro/bench_timing_wheel.cpp)
apex_add_benchmark(bench_slab_pool     micro/bench_slab_pool.cpp)
apex_add_benchmark(bench_session_lifecycle micro/bench_session_lifecycle.cpp)

# --- Integration benchmarks ---
apex_add_benchmark(bench_cross_core_latency       integration/bench_cross_core_latency.cpp)
apex_add_benchmark(bench_frame_pipeline            integration/bench_frame_pipeline.cpp)
apex_add_benchmark(bench_session_throughput         integration/bench_session_throughput.cpp)
apex_add_benchmark(bench_cross_core_message_passing integration/bench_cross_core_message_passing.cpp)
```

- [ ] **Step 3: apex_core/CMakeLists.txt에 benchmarks 서브디렉토리 추가**

`apex_core/CMakeLists.txt` 맨 끝 (`add_subdirectory(examples)` 이후)에 추가:
```cmake
add_subdirectory(benchmarks)
```

- [ ] **Step 4: 디렉토리 생성 + 빈 placeholder 파일 생성**

```bash
mkdir -p apex_core/benchmarks/micro apex_core/benchmarks/integration
```

아직 소스 파일이 없으므로 빌드는 Task 2에서 수행.

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json apex_core/vcpkg.json apex_core/CMakeLists.txt apex_core/benchmarks/CMakeLists.txt
git commit -m "build: Google Benchmark vcpkg 추가 + 벤치마크 CMake 인프라"
```

---

### Task 2: SystemProfile + bench_main + bench_helpers

**Files:**
- Create: `apex_core/benchmarks/system_profile.hpp`
- Create: `apex_core/benchmarks/bench_main.cpp`
- Create: `apex_core/benchmarks/bench_helpers.hpp`

- [ ] **Step 1: system_profile.hpp 작성**

```cpp
#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <sys/sysinfo.h>
#endif

namespace apex::bench {

struct SystemProfile {
    uint32_t physical_cores{0};
    uint32_t logical_cores{0};
    size_t total_ram_bytes{0};
    size_t available_ram_bytes{0};

    /// 벤치마크에 사용할 코어 수 (전체의 절반, 최소 1)
    uint32_t bench_cores() const {
        return std::max(1u, logical_cores > 2 ? logical_cores / 2 : logical_cores);
    }

    void print() const {
        std::cout << "=== System Profile ===\n"
                  << "Physical cores: " << physical_cores << '\n'
                  << "Logical cores:  " << logical_cores << '\n'
                  << "Total RAM:      " << (total_ram_bytes / (1024 * 1024)) << " MB\n"
                  << "Available RAM:  " << (available_ram_bytes / (1024 * 1024)) << " MB\n"
                  << "Bench cores:    " << bench_cores() << '\n'
                  << "======================\n";
    }
};

inline SystemProfile detect_system_profile() {
    SystemProfile p;
    p.logical_cores = std::thread::hardware_concurrency();

#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        std::vector<uint8_t> buf(len);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            uint32_t count = 0;
            DWORD offset = 0;
            while (offset < len) {
                auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    buf.data() + offset);
                if (info->Relationship == RelationProcessorCore) ++count;
                offset += info->Size;
            }
            p.physical_cores = count;
        }
    }
    if (p.physical_cores == 0) p.physical_cores = std::max(1u, p.logical_cores / 2);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        p.total_ram_bytes = mem.ullTotalPhys;
        p.available_ram_bytes = mem.ullAvailPhys;
    }
#else
    // Linux: /proc/cpuinfo의 첫 "cpu cores" 항목
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("cpu cores") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos)
                p.physical_cores = static_cast<uint32_t>(std::stoul(line.substr(pos + 1)));
            break;
        }
    }
    if (p.physical_cores == 0) p.physical_cores = std::max(1u, p.logical_cores / 2);

    struct sysinfo si {};
    if (sysinfo(&si) == 0) {
        p.total_ram_bytes = static_cast<size_t>(si.totalram) * si.mem_unit;
        p.available_ram_bytes = static_cast<size_t>(si.freeram + si.bufferram) * si.mem_unit;
    }
#endif

    return p;
}

} // namespace apex::bench
```

- [ ] **Step 2: bench_main.cpp 작성**

```cpp
#include <benchmark/benchmark.h>
#include "system_profile.hpp"

int main(int argc, char** argv) {
    auto profile = apex::bench::detect_system_profile();
    profile.print();

    benchmark::Initialize(&argc, argv);
    benchmark::AddCustomContext("physical_cores", std::to_string(profile.physical_cores));
    benchmark::AddCustomContext("logical_cores", std::to_string(profile.logical_cores));
    benchmark::AddCustomContext("total_ram_mb",
        std::to_string(profile.total_ram_bytes / (1024 * 1024)));

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
```

- [ ] **Step 3: bench_helpers.hpp 작성**

```cpp
#pragma once

#include <apex/core/wire_header.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <benchmark/benchmark.h>

#include <cstring>
#include <utility>
#include <vector>

namespace apex::bench {

/// TCP loopback 소켓 쌍 생성 (벤치마크 fixture용)
inline std::pair<boost::asio::ip::tcp::socket, boost::asio::ip::tcp::socket>
make_socket_pair(boost::asio::io_context& io) {
    using tcp = boost::asio::ip::tcp;
    tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), 0));
    auto port = acc.local_endpoint().port();

    tcp::socket client(io);
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    auto server = acc.accept();
    return {std::move(client), std::move(server)};
}

/// 완전한 와이어 프레임 (헤더 10B + payload) 빌드
inline std::vector<uint8_t> build_frame(uint16_t msg_id, size_t payload_size) {
    std::vector<uint8_t> frame(apex::core::WireHeader::SIZE + payload_size);

    apex::core::WireHeader header;
    header.msg_id = msg_id;
    header.body_size = static_cast<uint32_t>(payload_size);
    header.serialize(std::span<uint8_t, apex::core::WireHeader::SIZE>(
        frame.data(), apex::core::WireHeader::SIZE));

    // 페이로드를 패턴으로 채움
    std::memset(frame.data() + apex::core::WireHeader::SIZE, 0xAB, payload_size);
    return frame;
}

} // namespace apex::bench
```

- [ ] **Step 4: Commit**

```bash
git add apex_core/benchmarks/system_profile.hpp \
        apex_core/benchmarks/bench_main.cpp \
        apex_core/benchmarks/bench_helpers.hpp
git commit -m "feat(bench): SystemProfile 자동 감지 + bench_main + bench_helpers"
```

---

### Task 3: 첫 마이크로 벤치마크 — bench_mpsc_queue (proof of concept)

**Files:**
- Create: `apex_core/benchmarks/micro/bench_mpsc_queue.cpp`

- [ ] **Step 1: bench_mpsc_queue.cpp 작성**

```cpp
#include <apex/core/mpsc_queue.hpp>
#include <benchmark/benchmark.h>

#include <thread>

using namespace apex::core;

// --- 1P1C: 단일 producer-consumer throughput ---
static void BM_MpscQueue_1P1C(benchmark::State& state) {
    const auto capacity = static_cast<size_t>(state.range(0));
    MpscQueue<uint64_t> queue(capacity);

    for (auto _ : state) {
        auto res = queue.enqueue(42);
        benchmark::DoNotOptimize(res);
        auto val = queue.dequeue();
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MpscQueue_1P1C)->Range(1024, 65536);

// --- 2P1C: 2 producer threads + 1 consumer (벤치마크 스레드) ---
static void BM_MpscQueue_2P1C(benchmark::State& state) {
    MpscQueue<uint64_t> queue(65536);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> produced{0};

    std::jthread producer([&] {
        uint64_t val = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (queue.enqueue(val++).has_value())
                produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (auto _ : state) {
        queue.enqueue(99);
        while (auto val = queue.dequeue()) {
            benchmark::DoNotOptimize(val);
        }
    }

    running.store(false);
    state.SetItemsProcessed(state.iterations() + produced.load());
}
BENCHMARK(BM_MpscQueue_2P1C);

// --- 3P1C: 3 producer threads ---
static void BM_MpscQueue_3P1C(benchmark::State& state) {
    MpscQueue<uint64_t> queue(65536);
    std::atomic<bool> running{true};

    auto spawn_producer = [&] {
        return std::jthread([&] {
            uint64_t val = 0;
            while (running.load(std::memory_order_relaxed))
                queue.enqueue(val++);
        });
    };

    auto p1 = spawn_producer();
    auto p2 = spawn_producer();

    for (auto _ : state) {
        queue.enqueue(99);
        while (auto val = queue.dequeue()) {
            benchmark::DoNotOptimize(val);
        }
    }

    running.store(false);
}
BENCHMARK(BM_MpscQueue_3P1C);

// --- Backpressure: 큐 90% 찬 상태에서 enqueue/dequeue ---
static void BM_MpscQueue_Backpressure(benchmark::State& state) {
    MpscQueue<uint64_t> queue(1024);
    // 90% 채움
    for (size_t i = 0; i < 920; ++i)
        queue.enqueue(i);

    for (auto _ : state) {
        auto ok = queue.enqueue(42);
        benchmark::DoNotOptimize(ok);
        if (ok.has_value()) {
            auto val = queue.dequeue();
            benchmark::DoNotOptimize(val);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MpscQueue_Backpressure);
```

- [ ] **Step 2: 빌드**

```bash
# MSYS bash에서
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
```

Expected: 빌드 성공, `bench_mpsc_queue_debug.exe` 생성

- [ ] **Step 3: 실행 + 출력 확인**

```bash
./apex_core/bin/bench_mpsc_queue_debug --benchmark_format=console
```

Expected 출력 형태:
```
=== System Profile ===
Physical cores: 6
Logical cores:  12
...
-------------------------------------------------------
Benchmark                    Time    CPU   Iterations
-------------------------------------------------------
BM_MpscQueue_1P1C/1024     XX ns   XX ns     XXXXXXX
BM_MpscQueue_1P1C/65536    XX ns   XX ns     XXXXXXX
BM_MpscQueue_2P1C          XX ns   XX ns     XXXXXXX
BM_MpscQueue_3P1C          XX ns   XX ns     XXXXXXX
BM_MpscQueue_Backpressure  XX ns   XX ns     XXXXXXX
```

- [ ] **Step 4: JSON 출력도 확인**

```bash
./apex_core/bin/bench_mpsc_queue_debug \
    --benchmark_format=json \
    --benchmark_out=bench_mpsc_queue.json
```

JSON 파일에 `context.physical_cores`, `context.logical_cores`, `context.total_ram_mb` 필드가 포함되는지 확인.

- [ ] **Step 5: Commit**

```bash
git add apex_core/benchmarks/micro/bench_mpsc_queue.cpp
git commit -m "bench: MPSC 큐 마이크로벤치마크 (1P1C/2P1C/3P1C/backpressure)"
```

---

## Chunk 2: 마이크로 벤치마크 (Tasks 4–9)

### Task 4: bench_ring_buffer

**Files:**
- Create: `apex_core/benchmarks/micro/bench_ring_buffer.cpp`

- [ ] **Step 1: bench_ring_buffer.cpp 작성**

```cpp
#include <apex/core/ring_buffer.hpp>
#include <benchmark/benchmark.h>

#include <cstring>
#include <vector>

using namespace apex::core;

// --- Write + Read cycle (다양한 크기) ---
static void BM_RingBuffer_WriteRead(benchmark::State& state) {
    const auto chunk_size = static_cast<size_t>(state.range(0));
    RingBuffer buf(65536);
    std::vector<uint8_t> data(chunk_size, 0xAB);

    for (auto _ : state) {
        buf.write(data);
        auto readable = buf.contiguous_read();
        benchmark::DoNotOptimize(readable.data());
        buf.consume(readable.size());
    }
    state.SetBytesProcessed(state.iterations() * chunk_size);
}
BENCHMARK(BM_RingBuffer_WriteRead)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// --- Linearize 비용 (wrap-around 강제) ---
static void BM_RingBuffer_Linearize(benchmark::State& state) {
    const auto chunk_size = static_cast<size_t>(state.range(0));
    RingBuffer buf(4096);

    // 쓰기 위치를 buffer 끝 근처로 밀어 wrap-around 유도
    {
        std::vector<uint8_t> filler(4096 - chunk_size / 2, 0);
        buf.write(filler);
        buf.consume(filler.size());
    }

    std::vector<uint8_t> data(chunk_size, 0xCD);

    for (auto _ : state) {
        buf.write(data);
        auto span = buf.linearize(chunk_size);
        benchmark::DoNotOptimize(span.data());
        buf.consume(chunk_size);
    }
    state.SetBytesProcessed(state.iterations() * chunk_size);
}
BENCHMARK(BM_RingBuffer_Linearize)->Arg(64)->Arg(256)->Arg(1024);

// --- writable() span 획득 비용 ---
static void BM_RingBuffer_Writable(benchmark::State& state) {
    RingBuffer buf(65536);
    for (auto _ : state) {
        auto span = buf.writable();
        benchmark::DoNotOptimize(span.data());
        benchmark::DoNotOptimize(span.size());
    }
}
BENCHMARK(BM_RingBuffer_Writable);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_ring_buffer_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/micro/bench_ring_buffer.cpp
git commit -m "bench: RingBuffer 마이크로벤치마크 (write-read/linearize/writable)"
```

---

### Task 5: bench_frame_codec

**Files:**
- Create: `apex_core/benchmarks/micro/bench_frame_codec.cpp`

- [ ] **Step 1: bench_frame_codec.cpp 작성**

```cpp
#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>
#include <benchmark/benchmark.h>

#include <vector>

using namespace apex::core;

// --- Decode 지연 (다양한 페이로드 크기) ---
static void BM_FrameCodec_Decode(benchmark::State& state) {
    const auto payload_size = static_cast<size_t>(state.range(0));
    RingBuffer buf(payload_size + WireHeader::SIZE + 256);

    // 프레임 한 개를 미리 기록
    WireHeader header;
    header.msg_id = 0x0001;
    header.body_size = static_cast<uint32_t>(payload_size);
    auto header_bytes = header.serialize();

    std::vector<uint8_t> payload(payload_size, 0xAB);

    for (auto _ : state) {
        state.PauseTiming();
        buf.reset();
        buf.write(header_bytes);
        buf.write(payload);
        state.ResumeTiming();

        auto frame = FrameCodec::try_decode(buf);
        benchmark::DoNotOptimize(frame);
        if (frame.has_value())
            FrameCodec::consume_frame(buf, *frame);
    }
    state.SetBytesProcessed(state.iterations() * (WireHeader::SIZE + payload_size));
}
BENCHMARK(BM_FrameCodec_Decode)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(16384);

// --- Encode 지연 ---
static void BM_FrameCodec_Encode(benchmark::State& state) {
    const auto payload_size = static_cast<size_t>(state.range(0));
    RingBuffer buf(payload_size + WireHeader::SIZE + 256);

    WireHeader header;
    header.msg_id = 0x0001;
    header.body_size = static_cast<uint32_t>(payload_size);
    std::vector<uint8_t> payload(payload_size, 0xAB);

    for (auto _ : state) {
        buf.reset();
        auto ok = FrameCodec::encode(buf, header, payload);
        benchmark::DoNotOptimize(ok);
    }
    state.SetBytesProcessed(state.iterations() * (WireHeader::SIZE + payload_size));
}
BENCHMARK(BM_FrameCodec_Encode)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// --- encode_to (직접 span에 쓰기) ---
static void BM_FrameCodec_EncodeTo(benchmark::State& state) {
    const auto payload_size = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> out(WireHeader::SIZE + payload_size);
    std::vector<uint8_t> payload(payload_size, 0xAB);

    WireHeader header;
    header.msg_id = 0x0001;
    header.body_size = static_cast<uint32_t>(payload_size);

    for (auto _ : state) {
        auto written = FrameCodec::encode_to(out, header, payload);
        benchmark::DoNotOptimize(written);
    }
    state.SetBytesProcessed(state.iterations() * (WireHeader::SIZE + payload_size));
}
BENCHMARK(BM_FrameCodec_EncodeTo)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_frame_codec_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/micro/bench_frame_codec.cpp
git commit -m "bench: FrameCodec 마이크로벤치마크 (decode/encode/encode_to)"
```

---

### Task 6: bench_dispatcher

**Files:**
- Create: `apex_core/benchmarks/micro/bench_dispatcher.cpp`

- [ ] **Step 1: bench_dispatcher.cpp 작성**

MessageDispatcher::dispatch()는 코루틴이라 마이크로벤치마크에 부적합.
lookup 비용(has_handler)과 등록 비용만 측정한다.

```cpp
#include <apex/core/message_dispatcher.hpp>
#include <benchmark/benchmark.h>

using namespace apex::core;

// --- has_handler (hit) — 현재 O(1) 배열 인덱스 ---
static void BM_Dispatcher_LookupHit(benchmark::State& state) {
    const auto num_handlers = static_cast<int>(state.range(0));
    MessageDispatcher dispatcher;

    for (int i = 0; i < num_handlers; ++i) {
        dispatcher.register_handler(static_cast<uint16_t>(i),
            [](SessionPtr, uint16_t, std::span<const uint8_t>)
                -> boost::asio::awaitable<Result<void>> { co_return ok(); });
    }

    for (auto _ : state) {
        // 등록된 핸들러 중 하나를 조회
        auto hit = dispatcher.has_handler(static_cast<uint16_t>(num_handlers / 2));
        benchmark::DoNotOptimize(hit);
    }
}
BENCHMARK(BM_Dispatcher_LookupHit)->Arg(10)->Arg(100)->Arg(1000);

// --- has_handler (miss) ---
static void BM_Dispatcher_LookupMiss(benchmark::State& state) {
    MessageDispatcher dispatcher;
    dispatcher.register_handler(0x0001,
        [](SessionPtr, uint16_t, std::span<const uint8_t>)
            -> boost::asio::awaitable<Result<void>> { co_return ok(); });

    for (auto _ : state) {
        auto miss = dispatcher.has_handler(0xFFFF);
        benchmark::DoNotOptimize(miss);
    }
}
BENCHMARK(BM_Dispatcher_LookupMiss);

// --- register_handler 비용 ---
static void BM_Dispatcher_Register(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        MessageDispatcher dispatcher;
        state.ResumeTiming();

        for (uint16_t i = 0; i < 100; ++i) {
            dispatcher.register_handler(i,
                [](SessionPtr, uint16_t, std::span<const uint8_t>)
                    -> boost::asio::awaitable<Result<void>> { co_return ok(); });
        }
    }
    state.SetItemsProcessed(state.iterations() * 100);
}
BENCHMARK(BM_Dispatcher_Register);

// --- 65536-array 메모리: 생성 비용 (2MB heap alloc) ---
static void BM_Dispatcher_Construction(benchmark::State& state) {
    for (auto _ : state) {
        MessageDispatcher dispatcher;
        benchmark::DoNotOptimize(&dispatcher);
    }
}
BENCHMARK(BM_Dispatcher_Construction);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_dispatcher_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/micro/bench_dispatcher.cpp
git commit -m "bench: MessageDispatcher 마이크로벤치마크 (lookup/register/construction)"
```

---

### Task 7: bench_timing_wheel

**Files:**
- Create: `apex_core/benchmarks/micro/bench_timing_wheel.cpp`

- [ ] **Step 1: bench_timing_wheel.cpp 작성**

```cpp
#include <apex/core/timing_wheel.hpp>
#include <benchmark/benchmark.h>

using namespace apex::core;

// --- Schedule 처리량 ---
static void BM_TimingWheel_Schedule(benchmark::State& state) {
    const auto count = static_cast<size_t>(state.range(0));
    TimingWheel wheel(512, [](TimingWheel::EntryId) {});

    for (auto _ : state) {
        for (size_t i = 0; i < count; ++i)
            wheel.schedule(static_cast<uint32_t>(i % 512));

        // cleanup: tick 모두 소진
        state.PauseTiming();
        for (size_t i = 0; i < 512; ++i) wheel.tick();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_TimingWheel_Schedule)->Arg(1000)->Arg(10000)->Arg(50000);

// --- Tick 비용 (엔트리 N개 만료) ---
static void BM_TimingWheel_Tick(benchmark::State& state) {
    const auto entries_per_slot = static_cast<size_t>(state.range(0));
    size_t expired = 0;
    TimingWheel wheel(512, [&](TimingWheel::EntryId) { ++expired; });

    for (auto _ : state) {
        state.PauseTiming();
        // 현재 슬롯에 N개 엔트리 등록 (ticks_from_now=0 → 다음 tick에 만료)
        for (size_t i = 0; i < entries_per_slot; ++i)
            wheel.schedule(0);
        expired = 0;
        state.ResumeTiming();

        wheel.tick();
    }
    state.SetItemsProcessed(state.iterations() * entries_per_slot);
}
BENCHMARK(BM_TimingWheel_Tick)->Arg(10)->Arg(100)->Arg(1000);

// --- Reschedule 비용 ---
static void BM_TimingWheel_Reschedule(benchmark::State& state) {
    TimingWheel wheel(512, [](TimingWheel::EntryId) {});
    auto id = wheel.schedule(256);

    for (auto _ : state) {
        wheel.reschedule(id, 256);
    }
}
BENCHMARK(BM_TimingWheel_Reschedule);

// --- Cancel 비용 ---
static void BM_TimingWheel_Cancel(benchmark::State& state) {
    TimingWheel wheel(512, [](TimingWheel::EntryId) {});

    for (auto _ : state) {
        state.PauseTiming();
        auto id = wheel.schedule(256);
        state.ResumeTiming();

        wheel.cancel(id);
    }
}
BENCHMARK(BM_TimingWheel_Cancel);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_timing_wheel_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/micro/bench_timing_wheel.cpp
git commit -m "bench: TimingWheel 마이크로벤치마크 (schedule/tick/reschedule/cancel)"
```

---

### Task 8: bench_slab_pool

**Files:**
- Create: `apex_core/benchmarks/micro/bench_slab_pool.cpp`

- [ ] **Step 1: bench_slab_pool.cpp 작성**

```cpp
#include <apex/core/slab_pool.hpp>
#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace apex::core;

// --- SlabPool alloc + dealloc ---
static void BM_SlabPool_AllocDealloc(benchmark::State& state) {
    SlabPool pool(128, 4096);

    for (auto _ : state) {
        void* p = pool.allocate();
        benchmark::DoNotOptimize(p);
        pool.deallocate(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SlabPool_AllocDealloc);

// --- raw new/delete 비교 ---
static void BM_RawNew_AllocDealloc(benchmark::State& state) {
    for (auto _ : state) {
        auto* p = new char[128];
        benchmark::DoNotOptimize(p);
        delete[] p;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RawNew_AllocDealloc);

// --- make_shared 비교 ---
struct DummyObject {
    char data[128];
};

static void BM_MakeShared_AllocDealloc(benchmark::State& state) {
    for (auto _ : state) {
        auto p = std::make_shared<DummyObject>();
        benchmark::DoNotOptimize(p.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MakeShared_AllocDealloc);

// --- TypedSlabPool construct + destroy ---
static void BM_TypedSlabPool_ConstructDestroy(benchmark::State& state) {
    TypedSlabPool<DummyObject> pool(4096);

    for (auto _ : state) {
        auto* p = pool.construct();
        benchmark::DoNotOptimize(p);
        pool.destroy(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TypedSlabPool_ConstructDestroy);

// --- 대량 할당 패턴 (batch alloc → batch free) ---
static void BM_SlabPool_BatchAlloc(benchmark::State& state) {
    const auto batch = static_cast<size_t>(state.range(0));
    SlabPool pool(128, batch * 2);
    std::vector<void*> ptrs(batch);

    for (auto _ : state) {
        for (size_t i = 0; i < batch; ++i)
            ptrs[i] = pool.allocate();
        for (size_t i = 0; i < batch; ++i)
            pool.deallocate(ptrs[i]);
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_SlabPool_BatchAlloc)->Arg(100)->Arg(1000)->Arg(4096);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_slab_pool_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/micro/bench_slab_pool.cpp
git commit -m "bench: SlabPool 마이크로벤치마크 (slab vs new vs make_shared + batch)"
```

---

### Task 9: bench_session_lifecycle

**Files:**
- Create: `apex_core/benchmarks/micro/bench_session_lifecycle.cpp`

- [ ] **Step 1: bench_session_lifecycle.cpp 작성**

```cpp
#include <apex/core/session.hpp>
#include "bench_helpers.hpp"

#include <benchmark/benchmark.h>

using namespace apex::core;

// --- Session 생성 비용 (소켓 포함) ---
static void BM_Session_Create(benchmark::State& state) {
    boost::asio::io_context io;

    for (auto _ : state) {
        state.PauseTiming();
        auto [client, server] = apex::bench::make_socket_pair(io);
        state.ResumeTiming();

        auto session = std::make_shared<Session>(1, std::move(server), 0, 4096);
        benchmark::DoNotOptimize(session.get());

        state.PauseTiming();
        session->close();
        session.reset();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_Session_Create);

// --- shared_ptr 복사 비용 (atomic refcount) ---
static void BM_SessionPtr_Copy(benchmark::State& state) {
    boost::asio::io_context io;
    auto [client, server] = apex::bench::make_socket_pair(io);
    auto session = std::make_shared<Session>(1, std::move(server), 0, 4096);

    for (auto _ : state) {
        SessionPtr copy = session;  // atomic refcount increment
        benchmark::DoNotOptimize(copy.get());
        // copy 소멸 시 atomic decrement
    }
}
BENCHMARK(BM_SessionPtr_Copy);

// --- shared_ptr 생성 비용 (vs raw new, intrusive_ptr 전환 baseline) ---
static void BM_MakeShared_Session(benchmark::State& state) {
    boost::asio::io_context io;

    for (auto _ : state) {
        state.PauseTiming();
        auto [client, server] = apex::bench::make_socket_pair(io);
        state.ResumeTiming();

        auto p = std::make_shared<Session>(1, std::move(server), 0, 4096);
        benchmark::DoNotOptimize(p.get());

        state.PauseTiming();
        p->close();
        p.reset();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_MakeShared_Session);

// --- RingBuffer 단독 생성 비용 (Session 비용 분리) ---
static void BM_RingBuffer_Create(benchmark::State& state) {
    const auto capacity = static_cast<size_t>(state.range(0));
    for (auto _ : state) {
        RingBuffer buf(capacity);
        benchmark::DoNotOptimize(&buf);
    }
}
BENCHMARK(BM_RingBuffer_Create)->Arg(4096)->Arg(8192)->Arg(16384);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_session_lifecycle_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/micro/bench_session_lifecycle.cpp
git commit -m "bench: Session 라이프사이클 마이크로벤치마크 (create/ptr-copy/ringbuf)"
```

---

## Chunk 3: 통합 벤치마크 + CI + Baseline (Tasks 10–14)

### Task 10: bench_cross_core_latency

**Files:**
- Create: `apex_core/benchmarks/integration/bench_cross_core_latency.cpp`

- [ ] **Step 1: bench_cross_core_latency.cpp 작성**

Ping-pong RTT/2 패턴: Core 0 → Core 1 → Core 0 왕복 후 /2.
clock skew 없음 (같은 스레드에서 시작/종료 측정).

```cpp
#include <apex/core/core_engine.hpp>
#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <latch>

using namespace apex::core;

static void BM_CrossCore_PingPongRTT(benchmark::State& state) {
    const uint32_t num_cores = 2;
    CoreEngine engine({.num_cores = num_cores, .mpsc_queue_capacity = 65536});

    std::atomic<uint64_t> pong_count{0};
    std::latch ready(1);

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        if (msg.type == CoreMessage::Type::Custom) {
            if (core_id == 1) {
                // Core 1: pong back to Core 0
                engine.post_to(0, CoreMessage{
                    .type = CoreMessage::Type::Custom,
                    .source_core = 1,
                    .data = msg.data});
            } else if (core_id == 0) {
                // Core 0: pong 수신
                pong_count.fetch_add(1, std::memory_order_release);
            }
        }
    });

    engine.start();
    // drain_timer가 시작될 시간을 줌
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (auto _ : state) {
        auto before = pong_count.load(std::memory_order_acquire);
        engine.post_to(1, CoreMessage{
            .type = CoreMessage::Type::Custom,
            .source_core = 0,
            .data = 42});

        // pong 대기 (busy-wait — 벤치마크에서만 사용)
        while (pong_count.load(std::memory_order_acquire) == before) {
            std::this_thread::yield();
        }
    }

    engine.stop();
    engine.join();
    engine.drain_remaining();

    // RTT/2 = 편도 지연
    state.SetLabel("RTT/2");
}
BENCHMARK(BM_CrossCore_PingPongRTT)->Iterations(10000)->Unit(benchmark::kMicrosecond);

// --- Post throughput: fire-and-forget 대량 발사 ---
static void BM_CrossCore_PostThroughput(benchmark::State& state) {
    CoreEngine engine({.num_cores = 2, .mpsc_queue_capacity = 65536});
    std::atomic<uint64_t> received{0};

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        if (core_id == 1 && msg.type == CoreMessage::Type::Custom)
            received.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (auto _ : state) {
        engine.post_to(1, CoreMessage{
            .type = CoreMessage::Type::Custom,
            .source_core = 0,
            .data = 0});
    }

    // 잔여 메시지 소비 대기
    engine.stop();
    engine.join();
    engine.drain_remaining();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CrossCore_PostThroughput)->Unit(benchmark::kNanosecond);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_cross_core_latency_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/integration/bench_cross_core_latency.cpp
git commit -m "bench: cross-core 지연 통합벤치마크 (ping-pong RTT/2 + post throughput)"
```

---

### Task 11: bench_frame_pipeline

**Files:**
- Create: `apex_core/benchmarks/integration/bench_frame_pipeline.cpp`

- [ ] **Step 1: bench_frame_pipeline.cpp 작성**

네트워크 없이 decode → handler lookup → encode 전체 사이클 측정.

```cpp
#include <apex/core/frame_codec.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>
#include "bench_helpers.hpp"

#include <benchmark/benchmark.h>
#include <vector>

using namespace apex::core;

// --- Full pipeline: encode → decode → lookup → encode response ---
static void BM_FramePipeline_FullCycle(benchmark::State& state) {
    const auto payload_size = static_cast<size_t>(state.range(0));

    // 요청 프레임 미리 빌드
    auto request_frame = apex::bench::build_frame(0x0001, payload_size);

    // 응답 프레임 미리 빌드
    auto response_frame = apex::bench::build_frame(0x8001, payload_size);

    // dispatcher (lookup만 사용)
    MessageDispatcher dispatcher;
    dispatcher.register_handler(0x0001,
        [](SessionPtr, uint16_t, std::span<const uint8_t>)
            -> boost::asio::awaitable<Result<void>> { co_return ok(); });

    RingBuffer recv_buf(payload_size * 2 + 256);

    for (auto _ : state) {
        // 1. 수신 버퍼에 프레임 쓰기 (네트워크 수신 시뮬레이션)
        recv_buf.reset();
        recv_buf.write(request_frame);

        // 2. Decode
        auto frame = FrameCodec::try_decode(recv_buf);
        benchmark::DoNotOptimize(frame);

        // 3. Handler lookup (dispatch 대신 has_handler — 코루틴 비용 제외)
        auto hit = dispatcher.has_handler(frame->header.msg_id);
        benchmark::DoNotOptimize(hit);

        // 4. Consume
        FrameCodec::consume_frame(recv_buf, *frame);

        // 5. Response encode
        recv_buf.reset();
        auto ok = FrameCodec::encode(recv_buf, frame->header,
            std::span<const uint8_t>(response_frame.data() + WireHeader::SIZE, payload_size));
        benchmark::DoNotOptimize(ok);
    }
    state.SetBytesProcessed(state.iterations() * (WireHeader::SIZE + payload_size) * 2);
}
BENCHMARK(BM_FramePipeline_FullCycle)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_frame_pipeline_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/integration/bench_frame_pipeline.cpp
git commit -m "bench: 프레임 파이프라인 통합벤치마크 (encode→decode→lookup→encode)"
```

---

### Task 12: bench_cross_core_message_passing (closure shipping baseline)

**Files:**
- Create: `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp`

- [ ] **Step 1: bench_cross_core_message_passing.cpp 작성**

현재 closure shipping 방식의 baseline 측정.
Tier 1 구현 후 message passing 벤치마크 추가 예정.

```cpp
#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_call.hpp>
#include <benchmark/benchmark.h>

#include <atomic>

using namespace apex::core;

// --- Closure shipping: cross_core_post (현재 구현) ---
// new std::function per message — heap alloc 비용 포함
// NOTE: drain_timer가 CrossCorePost를 자동 실행+delete하므로
//       별도 message_handler 불필요.
//       측정값에는 drain_interval(100μs 기본) 대기 시간이 포함됨.
//       Tier 1의 post()+atomic drain 전환 후 이 오버헤드가 제거되는 것이 핵심.
static void BM_CrossCore_ClosureShipping(benchmark::State& state) {
    CoreEngine engine({.num_cores = 2, .mpsc_queue_capacity = 65536});
    std::atomic<uint64_t> processed{0};

    // message_handler는 Custom 타입만 호출됨.
    // CrossCorePost는 drain_timer가 자동 처리.
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (auto _ : state) {
        cross_core_post(engine, 0, 1, [&] {
            processed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    engine.stop();
    engine.join();
    engine.drain_remaining();

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("closure-shipping");
}
BENCHMARK(BM_CrossCore_ClosureShipping)->Unit(benchmark::kNanosecond);

// --- Broadcast: per-core memcpy baseline ---
// 모든 코어에 데이터 복사 전송 (현재 패턴)
static void BM_Broadcast_PerCoreCopy(benchmark::State& state) {
    const auto num_cores = static_cast<uint32_t>(state.range(0));
    CoreEngine engine({.num_cores = num_cores, .mpsc_queue_capacity = 65536});
    std::atomic<uint64_t> received{0};

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        if (msg.type == CoreMessage::Type::Custom)
            received.fetch_add(1, std::memory_order_relaxed);
    });

    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (auto _ : state) {
        // 모든 코어에 broadcast
        engine.broadcast(CoreMessage{
            .type = CoreMessage::Type::Custom,
            .source_core = 0,
            .data = 42});
    }

    engine.stop();
    engine.join();
    engine.drain_remaining();

    state.SetItemsProcessed(state.iterations() * num_cores);
}
BENCHMARK(BM_Broadcast_PerCoreCopy)->Arg(2)->Arg(4);

// TODO: Phase 5.5 Tier 1 구현 후 추가:
// BENCHMARK(BM_CrossCore_MessagePassing);     // op 기반 handler dispatch
// BENCHMARK(BM_CrossCore_RequestResponse);    // message passing request-reply
// BENCHMARK(BM_Broadcast_SharedImmutable);    // immutable shared payload
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_cross_core_message_passing_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp
git commit -m "bench: cross-core message passing baseline (closure shipping + broadcast)"
```

---

### Task 13: bench_session_throughput

**Files:**
- Create: `apex_core/benchmarks/integration/bench_session_throughput.cpp`

- [ ] **Step 1: bench_session_throughput.cpp 작성**

TCP loopback 소켓 쌍으로 동기 echo round-trip 측정.

```cpp
#include <apex/core/wire_header.hpp>
#include "bench_helpers.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <benchmark/benchmark.h>

using namespace apex::core;

// --- TCP echo round-trip (동기 I/O, 프레임워크 오버헤드 제외) ---
static void BM_Session_TcpEchoRoundTrip(benchmark::State& state) {
    const auto payload_size = static_cast<size_t>(state.range(0));
    boost::asio::io_context io;

    auto [client, server] = apex::bench::make_socket_pair(io);
    auto frame = apex::bench::build_frame(0x0001, payload_size);
    std::vector<uint8_t> read_buf(frame.size());

    for (auto _ : state) {
        // Client → Server
        boost::asio::write(client, boost::asio::buffer(frame));
        // Server ← (read)
        boost::asio::read(server, boost::asio::buffer(read_buf));
        // Server → Client (echo)
        boost::asio::write(server, boost::asio::buffer(read_buf));
        // Client ← (read)
        boost::asio::read(client, boost::asio::buffer(read_buf));
        benchmark::DoNotOptimize(read_buf.data());
    }
    state.SetBytesProcessed(state.iterations() * frame.size() * 2);
    state.SetLabel("sync-loopback");
}
BENCHMARK(BM_Session_TcpEchoRoundTrip)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// --- TCP write-only throughput (단방향) ---
static void BM_Session_TcpWriteThroughput(benchmark::State& state) {
    const auto payload_size = static_cast<size_t>(state.range(0));
    boost::asio::io_context io;

    auto [client, server] = apex::bench::make_socket_pair(io);
    auto frame = apex::bench::build_frame(0x0001, payload_size);

    // server 측에서 drain (별도 스레드)
    std::atomic<bool> running{true};
    std::jthread drainer([&] {
        std::vector<uint8_t> buf(65536);
        boost::system::error_code ec;
        while (running.load(std::memory_order_relaxed)) {
            server.read_some(boost::asio::buffer(buf), ec);
            if (ec) break;
        }
    });

    for (auto _ : state) {
        boost::asio::write(client, boost::asio::buffer(frame));
    }
    state.SetBytesProcessed(state.iterations() * frame.size());

    running.store(false);
    client.close();
}
BENCHMARK(BM_Session_TcpWriteThroughput)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);
```

- [ ] **Step 2: 빌드 + 실행**

```bash
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
./apex_core/bin/bench_session_throughput_debug --benchmark_format=console
```

- [ ] **Step 3: Commit**

```bash
git add apex_core/benchmarks/integration/bench_session_throughput.cpp
git commit -m "bench: TCP 세션 처리량 통합벤치마크 (echo round-trip + write throughput)"
```

---

### Task 14: CMake preset + CI + baseline 측정

**Files:**
- Modify: `CMakePresets.json`
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: CMakePresets.json에 benchmark preset 추가**

configure presets에 추가:
```json
{
    "name": "benchmark",
    "inherits": "default",
    "displayName": "Benchmark (Release)",
    "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "APEX_BUILD_VARIANT": "benchmark"
    }
}
```

build presets에 추가:
```json
{
    "name": "benchmark",
    "configurePreset": "benchmark"
}
```

- [ ] **Step 2: CI에 벤치마크 빌드 검증 추가**

`.github/workflows/ci.yml`의 `build` job matrix에 벤치마크 빌드 타겟 추가는 불필요 —
기존 `debug` preset 빌드가 `add_subdirectory(benchmarks)`를 포함하므로 자동으로 빌드됨.

CI에서 벤치마크가 빌드되는지 확인하려면 `linux-gcc` 빌드 로그에서 `Building benchmarks` 메시지를 확인.

- [ ] **Step 3: Release 빌드 + baseline JSON 측정**

```bash
# Release 빌드 (정확한 측정을 위해)
cmake --preset benchmark
cmake --build build/Windows/benchmark

# baseline 저장 디렉토리
mkdir -p apex_core/benchmarks/baselines

# 각 벤치마크 JSON 생성
for bench in bench_mpsc_queue bench_ring_buffer bench_frame_codec \
             bench_dispatcher bench_timing_wheel bench_slab_pool \
             bench_session_lifecycle bench_cross_core_latency \
             bench_frame_pipeline bench_session_throughput \
             bench_cross_core_message_passing; do
    ./apex_core/bin/${bench}_benchmark \
        --benchmark_format=json \
        --benchmark_out=apex_core/benchmarks/baselines/${bench}.json \
        --benchmark_repetitions=3 \
        --benchmark_report_aggregates_only=true
done
```

`--benchmark_repetitions=3`으로 3회 반복 후 평균/중앙값/표준편차를 JSON에 포함.

- [ ] **Step 4: .gitignore에 baseline 결과 추가**

baselines/ 디렉토리는 git에 추적하되, 로컬 임시 결과는 제외:
```
# apex_core/benchmarks/.gitignore
*.json.tmp
```

baseline JSON 파일 자체는 커밋하여 before/after 비교에 사용.

- [ ] **Step 5: Commit**

```bash
git add CMakePresets.json \
        apex_core/benchmarks/baselines/ \
        apex_core/benchmarks/.gitignore
git commit -m "bench: Release preset 추가 + Phase 5.5 baseline JSON 측정"
```

---

## Summary

| Task | 내용 | 산출물 |
|------|------|--------|
| 1 | vcpkg + CMake 인프라 | 빌드 설정 |
| 2 | SystemProfile + bench_main | 공유 인프라 |
| 3 | bench_mpsc_queue (PoC) | micro/1 |
| 4 | bench_ring_buffer | micro/2 |
| 5 | bench_frame_codec | micro/3 |
| 6 | bench_dispatcher | micro/4 |
| 7 | bench_timing_wheel | micro/5 |
| 8 | bench_slab_pool | micro/6 |
| 9 | bench_session_lifecycle | micro/7 |
| 10 | bench_cross_core_latency | integration/1 |
| 11 | bench_frame_pipeline | integration/2 |
| 12 | bench_cross_core_message_passing | integration/3 |
| 13 | bench_session_throughput | integration/4 |
| 14 | CMake preset + CI + baseline | 인프라 + JSON |

**총 신규 파일**: 15개
**총 수정 파일**: 5개
**예상 신규 코드**: ~900줄 (C++) + ~50줄 (CMake/JSON)
