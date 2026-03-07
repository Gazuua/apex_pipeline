# Phase 1 + 2: Project Setup & Foundation Components Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Apex Core 프레임워크의 빌드 시스템을 구축하고, 4개의 기반 컴포넌트(MPSC 큐, 슬랩 풀, 링 버퍼, 타이머 휠)를 TDD로 구현한다.

**Architecture:** 모노레포 구조의 core/ 디렉토리에 C++20 프레임워크를 구축한다. vcpkg로 의존성을 관리하고, CMakePresets.json으로 원커맨드 빌드를 지원한다. Phase 2의 4개 컴포넌트는 Phase 1에서 정의된 헤더 인터페이스를 "계약"으로 삼아 에이전트 팀 4병렬로 구현한다.

**Tech Stack:** C++20, CMake 3.20+, Ninja, vcpkg, Google Test, Google Benchmark

**Design Docs:** `design-decisions.md`, `design-rationale.md` (ADR 1-23)

---

## Phase 1: Project Setup (단일 세션)

### Task 1.1: 디렉토리 스캐폴딩

**Files:**
- Create: `core/CMakeLists.txt`
- Create: `core/include/apex/core/.gitkeep`
- Create: `core/src/.gitkeep`
- Create: `core/tests/unit/.gitkeep`
- Create: `core/tests/integration/.gitkeep`
- Create: `core/tests/bench/.gitkeep`
- Create: `core/examples/.gitkeep`
- Create: `docs/plans/.gitkeep`
- Create: `docs/progress/.gitkeep`

**Step 1: Create directory structure**

```bash
cd D:/.workspace/BoostAsioCore
mkdir -p core/include/apex/core
mkdir -p core/src
mkdir -p core/tests/unit
mkdir -p core/tests/integration
mkdir -p core/tests/bench
mkdir -p core/examples
mkdir -p docs/plans
mkdir -p docs/progress
touch core/include/apex/core/.gitkeep
touch core/src/.gitkeep
touch core/tests/unit/.gitkeep
touch core/tests/integration/.gitkeep
touch core/tests/bench/.gitkeep
touch core/examples/.gitkeep
```

**Step 2: Verify structure**

```bash
find core -type d | sort
```

Expected:
```
core
core/examples
core/include
core/include/apex
core/include/apex/core
core/src
core/tests
core/tests/bench
core/tests/integration
core/tests/unit
```

---

### Task 1.2: vcpkg.json + CMakePresets.json

**Files:**
- Create: `vcpkg.json`
- Create: `CMakePresets.json`

**Step 1: Create vcpkg.json**

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "apex-pipeline",
  "version-semver": "0.1.0",
  "description": "Apex Pipeline - High-performance real-time server framework",
  "dependencies": [
    {
      "name": "boost-asio",
      "version>=": "1.84.0"
    },
    {
      "name": "boost-beast",
      "version>=": "1.84.0"
    },
    "flatbuffers",
    "spdlog",
    "tomlplusplus",
    {
      "name": "gtest",
      "host": true
    },
    {
      "name": "benchmark",
      "host": true
    }
  ]
}
```

Note: librdkafka, redis-plus-plus, libpq, prometheus-cpp, jwt-cpp는 Phase 5~6에서 추가. 현재는 Phase 1~2에 필요한 것만.

**Step 2: Create CMakePresets.json**

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 20, "patch": 0 },
  "configurePresets": [
    {
      "name": "default",
      "displayName": "Default (Release)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_CXX_STANDARD": "20",
        "CMAKE_CXX_STANDARD_REQUIRED": "ON",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      }
    },
    {
      "name": "debug",
      "displayName": "Debug",
      "inherits": "default",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug"
      }
    },
    {
      "name": "asan",
      "displayName": "Debug + AddressSanitizer",
      "inherits": "debug",
      "cacheVariables": {
        "CMAKE_CXX_FLAGS": "-fsanitize=address -fno-omit-frame-pointer"
      }
    },
    {
      "name": "tsan",
      "displayName": "Debug + ThreadSanitizer",
      "inherits": "debug",
      "cacheVariables": {
        "CMAKE_CXX_FLAGS": "-fsanitize=thread"
      }
    }
  ],
  "buildPresets": [
    { "name": "default", "configurePreset": "default" },
    { "name": "debug", "configurePreset": "debug" },
    { "name": "asan", "configurePreset": "asan" },
    { "name": "tsan", "configurePreset": "tsan" }
  ],
  "testPresets": [
    {
      "name": "default",
      "configurePreset": "default",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "debug",
      "configurePreset": "debug",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

**Step 3: Verify vcpkg is available**

```bash
echo $VCPKG_ROOT
```

Expected: vcpkg 설치 경로 (없으면 설치 필요)

---

### Task 1.3: CMake 빌드 시스템

**Files:**
- Create: `CMakeLists.txt` (최상위)
- Create: `core/CMakeLists.txt`
- Create: `core/tests/CMakeLists.txt`
- Create: `core/tests/unit/CMakeLists.txt`

**Step 1: Create root CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(apex-pipeline
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Apex Pipeline - High-performance real-time server framework"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# io_uring option (default OFF, Linux only)
option(APEX_USE_IO_URING "Enable io_uring backend for Boost.Asio" OFF)

add_subdirectory(core)
```

**Step 2: Create core/CMakeLists.txt**

```cmake
add_library(apex_core INTERFACE)
add_library(apex::core ALIAS apex_core)

target_include_directories(apex_core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(apex_core INTERFACE cxx_std_20)

# Phase 2 components are header-only for now
# Source files will be added as implementation progresses

enable_testing()
add_subdirectory(tests)
```

**Step 3: Create core/tests/CMakeLists.txt**

```cmake
find_package(GTest CONFIG REQUIRED)

add_subdirectory(unit)
# add_subdirectory(integration)  # Phase 3.5
# add_subdirectory(bench)        # Phase 2 (after unit tests pass)
```

**Step 4: Create core/tests/unit/CMakeLists.txt**

```cmake
# Each test file is added as components are implemented

function(apex_add_unit_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        PRIVATE
            apex::core
            GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# Phase 2 tests (uncomment as implemented):
# apex_add_unit_test(test_mpsc_queue test_mpsc_queue.cpp)
# apex_add_unit_test(test_slab_pool test_slab_pool.cpp)
# apex_add_unit_test(test_ring_buffer test_ring_buffer.cpp)
# apex_add_unit_test(test_timing_wheel test_timing_wheel.cpp)
```

**Step 5: Verify build system works**

```bash
cd D:/.workspace/BoostAsioCore
cmake --preset debug
cmake --build build/debug
```

Expected: 빌드 성공 (아직 소스 파일 없으므로 경고 없이 완료)

**Step 6: Commit**

```bash
git init
git add CMakeLists.txt CMakePresets.json vcpkg.json core/ docs/
git commit -m "chore: Phase 1.3 - CMake build system with vcpkg"
```

---

### Task 1.4: 헤더 인터페이스 정의 — MPSC Queue

**Files:**
- Create: `core/include/apex/core/mpsc_queue.hpp`

**Step 1: Write MPSC Queue interface**

설계 요건:
- 락프리, bounded (max_capacity)
- 캐시 라인 패딩 (alignas(64))
- acquire/release 메모리 오더링
- enqueue: std::expected<void, BackpressureError> 반환
- dequeue: std::optional<T> 반환
- 여러 Producer, 단일 Consumer

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <type_traits>

namespace apex::core {

enum class QueueError : uint8_t {
    Full,
};

/// Lock-free bounded MPSC (Multi-Producer, Single-Consumer) queue.
/// Designed for inter-core communication in shared-nothing architecture.
///
/// - Multiple producers can enqueue concurrently (lock-free via CAS).
/// - Single consumer dequeues (wait-free).
/// - Fixed capacity set at construction time.
/// - Cache-line aligned to prevent false sharing.
///
/// Template parameter T must be trivially copyable for lock-free guarantees.
template <typename T>
    requires std::is_trivially_copyable_v<T>
class alignas(64) MpscQueue {
public:
    /// Constructs a queue with the given maximum capacity.
    /// @param capacity Must be > 0. Rounded up to next power of 2 internally.
    explicit MpscQueue(size_t capacity);

    ~MpscQueue();

    // Non-copyable, non-movable (aligned, owns memory)
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    /// Thread-safe. Lock-free. Called by any producer core.
    /// @return QueueError::Full if queue is at capacity (backpressure).
    [[nodiscard]] std::expected<void, QueueError> enqueue(const T& item);

    /// NOT thread-safe. Called only by the owning consumer core.
    /// @return std::nullopt if queue is empty.
    [[nodiscard]] std::optional<T> dequeue();

    /// Thread-safe. Approximate count (may be stale).
    [[nodiscard]] size_t size_approx() const noexcept;

    /// Thread-safe.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Thread-safe. Approximate check.
    [[nodiscard]] bool empty() const noexcept;

private:
    struct alignas(64) Slot {
        std::atomic<bool> ready{false};
        T data;
    };

    Slot* slots_;
    size_t capacity_;
    size_t mask_;  // capacity_ - 1 (power of 2)

    alignas(64) std::atomic<size_t> head_{0};  // producer CAS target
    alignas(64) size_t tail_{0};               // consumer-only, no atomic needed
};

} // namespace apex::core
```

**Step 2: Commit**

```bash
git add core/include/apex/core/mpsc_queue.hpp
git commit -m "feat: Phase 1.4 - MPSC queue header interface"
```

---

### Task 1.5: 헤더 인터페이스 정의 — Slab Pool

**Files:**
- Create: `core/include/apex/core/slab_pool.hpp`

**Step 1: Write Slab Pool interface**

설계 요건:
- 코어별 독립, 타입별 풀
- O(1) 할당/해제
- alignas(64)
- 코루틴 프레임 전용 슬랩 지원 (promise_type에서 호출)

```cpp
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace apex::core {

/// Fixed-size slab memory pool for O(1) allocation/deallocation.
/// Designed for per-core use (no thread synchronization).
///
/// Memory is pre-allocated in chunks. Each chunk contains N slots of
/// fixed size. Free slots are tracked via an intrusive free-list.
///
/// Usage:
///   SlabPool pool(sizeof(MyObject), 1024);  // 1024 slots
///   void* p = pool.allocate();
///   pool.deallocate(p);
class SlabPool {
public:
    /// @param slot_size Size of each slot in bytes (will be aligned up to alignof(max_align_t)).
    /// @param initial_count Number of slots to pre-allocate.
    SlabPool(size_t slot_size, size_t initial_count);

    ~SlabPool();

    // Non-copyable, non-movable
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;
    SlabPool(SlabPool&&) = delete;
    SlabPool& operator=(SlabPool&&) = delete;

    /// O(1) allocation from the free-list. NOT thread-safe (per-core use).
    /// @return nullptr if pool is exhausted.
    [[nodiscard]] void* allocate();

    /// O(1) deallocation. Returns the slot to the free-list. NOT thread-safe.
    /// @param ptr Must have been allocated from this pool.
    void deallocate(void* ptr) noexcept;

    /// Number of currently allocated (in-use) slots.
    [[nodiscard]] size_t allocated_count() const noexcept;

    /// Number of free slots available.
    [[nodiscard]] size_t free_count() const noexcept;

    /// Total capacity (allocated + free).
    [[nodiscard]] size_t total_count() const noexcept;

    /// Size of each slot in bytes (after alignment).
    [[nodiscard]] size_t slot_size() const noexcept;

private:
    struct FreeNode {
        FreeNode* next;
    };

    void grow(size_t count);

    uint8_t* chunk_;         // raw memory block
    FreeNode* free_list_;    // head of free-list
    size_t slot_size_;       // aligned slot size
    size_t total_count_;     // total slots ever created
    size_t free_count_;      // current free slots
};

/// Typed wrapper around SlabPool for type-safe allocation.
/// Usage:
///   TypedSlabPool<Session> pool(1024);
///   Session* s = pool.construct(args...);
///   pool.destroy(s);
template <typename T>
class TypedSlabPool {
public:
    explicit TypedSlabPool(size_t initial_count)
        : pool_(sizeof(T), initial_count) {}

    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        void* p = pool_.allocate();
        if (!p) return nullptr;
        return new (p) T(std::forward<Args>(args)...);
    }

    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();
            pool_.deallocate(ptr);
        }
    }

    [[nodiscard]] size_t allocated_count() const noexcept { return pool_.allocated_count(); }
    [[nodiscard]] size_t free_count() const noexcept { return pool_.free_count(); }

private:
    SlabPool pool_;
};

} // namespace apex::core
```

**Step 2: Commit**

```bash
git add core/include/apex/core/slab_pool.hpp
git commit -m "feat: Phase 1.5 - Slab pool header interface"
```

---

### Task 1.6: 헤더 인터페이스 정의 — Ring Buffer

**Files:**
- Create: `core/include/apex/core/ring_buffer.hpp`

**Step 1: Write Ring Buffer interface**

설계 요건:
- memmove 없는 수신 버퍼
- 연속 읽기 영역 제공 (zero-copy FlatBuffers 접근)
- wrap-around 시 예외적 copy 허용
- 단일 스레드 사용 (코어별)

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace apex::core {

/// Circular buffer for network receive buffering.
/// Designed for per-core use (no thread synchronization).
///
/// Key design:
/// - No memmove: data stays in place, read/write positions advance.
/// - contiguous_read() returns the largest contiguous readable span.
/// - For FlatBuffers zero-copy: if the message fits in contiguous area, no copy.
///   If it wraps around, linearize() copies to make it contiguous.
class RingBuffer {
public:
    /// @param capacity Buffer size in bytes. Rounded up to next power of 2.
    explicit RingBuffer(size_t capacity);

    ~RingBuffer();

    // Non-copyable, non-movable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    /// Returns a writable span for the next available write area.
    /// Use commit_write() after writing data to advance the write position.
    [[nodiscard]] std::span<uint8_t> writable() noexcept;

    /// Advances write position by `n` bytes. Must be <= writable().size().
    void commit_write(size_t n) noexcept;

    /// Returns the largest contiguous readable span starting from read position.
    /// May be less than readable_size() if data wraps around.
    [[nodiscard]] std::span<const uint8_t> contiguous_read() const noexcept;

    /// Total number of readable bytes (may span wrap-around boundary).
    [[nodiscard]] size_t readable_size() const noexcept;

    /// Advances read position by `n` bytes. Must be <= readable_size().
    void consume(size_t n) noexcept;

    /// If the next `n` bytes are contiguous, returns a span to them directly (zero-copy).
    /// If they wrap around, copies them into an internal linearization buffer
    /// and returns a span to that buffer.
    /// @return span of exactly `n` bytes, or empty span if readable_size() < n.
    [[nodiscard]] std::span<const uint8_t> linearize(size_t n);

    /// Total buffer capacity.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Available space for writing.
    [[nodiscard]] size_t writable_size() const noexcept;

    /// Reset read/write positions to start.
    void reset() noexcept;

private:
    uint8_t* buffer_;
    uint8_t* linear_buf_;     // linearization scratch buffer (allocated on first use)
    size_t capacity_;
    size_t mask_;             // capacity_ - 1
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t linear_buf_size_{0};
};

} // namespace apex::core
```

**Step 2: Commit**

```bash
git add core/include/apex/core/ring_buffer.hpp
git commit -m "feat: Phase 1.6 - Ring buffer header interface"
```

---

### Task 1.7: 헤더 인터페이스 정의 — Timing Wheel

**Files:**
- Create: `core/include/apex/core/timing_wheel.hpp`

**Step 1: Write Timing Wheel interface**

설계 요건:
- 코어별 1개, O(1) 타임아웃 관리
- 슬롯 수와 틱 해상도 설정 가능
- 하트비트 수신 시 세션을 현재 틱 + timeout 위치로 이동
- 단일 스레드 사용 (코어별)
- 콜백 기반 만료 알림

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace apex::core {

/// Hierarchical timing wheel for O(1) timeout management.
/// Designed for per-core use (no thread synchronization).
///
/// Usage with Asio: a single steady_timer drives tick() at fixed intervals.
/// Sessions are added with a timeout. On each tick, expired entries fire callbacks.
///
/// Typical configuration: 512 slots, 1-second tick = ~8.5 minute max timeout.
class TimingWheel {
public:
    using EntryId = uint64_t;
    using Callback = std::function<void(EntryId)>;

    /// @param num_slots Number of slots in the wheel (rounded up to power of 2).
    /// @param on_expire Callback invoked for each expired entry.
    TimingWheel(size_t num_slots, Callback on_expire);

    ~TimingWheel();

    // Non-copyable, non-movable
    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;
    TimingWheel(TimingWheel&&) = delete;
    TimingWheel& operator=(TimingWheel&&) = delete;

    /// Add an entry that expires after `ticks_from_now` ticks.
    /// @return EntryId for later cancel/reschedule.
    [[nodiscard]] EntryId schedule(uint32_t ticks_from_now);

    /// Cancel a previously scheduled entry. O(1).
    /// No-op if already expired or cancelled.
    void cancel(EntryId id);

    /// Reschedule an existing entry to expire after `ticks_from_now` ticks.
    /// Equivalent to cancel + schedule but reuses the same id.
    void reschedule(EntryId id, uint32_t ticks_from_now);

    /// Advance the wheel by one tick. Fires on_expire for all entries in the current slot.
    /// Called by Asio steady_timer at fixed intervals.
    void tick();

    /// Number of currently active (non-expired, non-cancelled) entries.
    [[nodiscard]] size_t active_count() const noexcept;

    /// Current tick position in the wheel.
    [[nodiscard]] uint64_t current_tick() const noexcept;

private:
    struct Entry {
        EntryId id;
        uint64_t deadline_tick;
        bool cancelled{false};
        Entry* next{nullptr};
        Entry* prev{nullptr};
    };

    struct Slot {
        Entry* head{nullptr};
    };

    void insert_entry(Entry* entry, size_t slot_idx);
    void remove_entry(Entry* entry, size_t slot_idx);

    std::vector<Slot> slots_;
    size_t num_slots_;
    size_t mask_;
    uint64_t current_tick_{0};
    EntryId next_id_{1};
    Callback on_expire_;

    // Entry storage (pool-friendly)
    std::vector<Entry*> entries_;  // indexed by id for O(1) lookup
};

} // namespace apex::core
```

**Step 2: Commit**

```bash
git add core/include/apex/core/timing_wheel.hpp
git commit -m "feat: Phase 1.7 - Timing wheel header interface"
```

---

### Task 1.8: Phase 1 완료 검증

**Step 1: Full build verification**

```bash
cd D:/.workspace/BoostAsioCore
cmake --preset debug
cmake --build build/debug
```

Expected: 빌드 성공

**Step 2: Write Phase 1 checkpoint**

파일 `docs/progress/phase-1-complete.md` 작성:
```markdown
# Phase 1 Complete

## 완료 항목
- 디렉토리 구조 생성
- vcpkg.json (GTest, Boost, FlatBuffers, spdlog, toml++)
- CMakePresets.json (default, debug, asan, tsan)
- CMake 빌드 시스템 (root + core + tests)
- 헤더 인터페이스 4개:
  - MpscQueue<T> - 락프리 bounded MPSC 큐
  - SlabPool / TypedSlabPool<T> - O(1) 슬랩 메모리 풀
  - RingBuffer - zero-copy 수신 버퍼
  - TimingWheel - O(1) 타임아웃 관리

## Phase 2 병렬 작업 준비
각 에이전트는 해당 헤더의 구현(.cpp) + 테스트를 담당.
헤더 인터페이스를 변경하지 말 것 (계약).
```

**Step 3: Commit**

```bash
git add -A
git commit -m "chore: Phase 1 complete - project setup and header interfaces"
```

---

## Phase 2: Foundation Components (에이전트 팀 4병렬)

Phase 2부터는 4개 에이전트가 독립적으로 작업한다. 각 에이전트의 작업은 파일이 겹치지 않는다.

**공통 규칙:**
- 헤더 인터페이스(Phase 1에서 정의)를 변경하지 않는다
- 구현 파일: `core/src/<component>.cpp`
- 테스트 파일: `core/tests/unit/test_<component>.cpp`
- 테스트 통과 후 커밋

---

### Task 2A: MPSC Lock-Free Queue (Agent A)

**Files:**
- Create: `core/src/mpsc_queue.cpp` (or header-only in .hpp)
- Create: `core/tests/unit/test_mpsc_queue.cpp`
- Modify: `core/tests/unit/CMakeLists.txt` (uncomment test line)

Note: MpscQueue는 템플릿이므로 header-only 구현이 자연스러움. 구현을 `mpsc_queue.hpp`에 직접 추가.

**Step 1: Write failing tests**

```cpp
// core/tests/unit/test_mpsc_queue.cpp
#include <apex/core/mpsc_queue.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <set>

using namespace apex::core;

// --- Basic functionality ---

TEST(MpscQueue, ConstructWithCapacity) {
    MpscQueue<int> q(16);
    EXPECT_EQ(q.capacity(), 16u);
    EXPECT_EQ(q.size_approx(), 0u);
    EXPECT_TRUE(q.empty());
}

TEST(MpscQueue, CapacityRoundsUpToPowerOfTwo) {
    MpscQueue<int> q(10);
    EXPECT_EQ(q.capacity(), 16u);  // next power of 2
}

TEST(MpscQueue, EnqueueDequeue_SingleItem) {
    MpscQueue<int> q(4);
    auto result = q.enqueue(42);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(q.size_approx(), 1u);

    auto item = q.dequeue();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, 42);
    EXPECT_TRUE(q.empty());
}

TEST(MpscQueue, EnqueueDequeue_FIFO) {
    MpscQueue<int> q(8);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(q.enqueue(i).has_value());
    }
    for (int i = 0; i < 5; ++i) {
        auto item = q.dequeue();
        ASSERT_TRUE(item.has_value());
        EXPECT_EQ(*item, i);
    }
}

TEST(MpscQueue, DequeueEmpty_ReturnsNullopt) {
    MpscQueue<int> q(4);
    EXPECT_FALSE(q.dequeue().has_value());
}

TEST(MpscQueue, Backpressure_WhenFull) {
    MpscQueue<int> q(4);
    for (size_t i = 0; i < q.capacity(); ++i) {
        ASSERT_TRUE(q.enqueue(static_cast<int>(i)).has_value());
    }
    // Queue is full
    auto result = q.enqueue(999);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), QueueError::Full);
}

TEST(MpscQueue, WrapAround) {
    MpscQueue<int> q(4);
    // Fill and drain twice to test wrap-around
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.enqueue(round * 10 + i).has_value());
        }
        for (int i = 0; i < 4; ++i) {
            auto item = q.dequeue();
            ASSERT_TRUE(item.has_value());
            EXPECT_EQ(*item, round * 10 + i);
        }
    }
}

// --- Concurrency ---

TEST(MpscQueue, MultiProducerSingleConsumer) {
    constexpr int kNumProducers = 4;
    constexpr int kItemsPerProducer = 10000;
    MpscQueue<int> q(4096);

    std::vector<std::thread> producers;
    for (int p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&q, p]() {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                int value = p * kItemsPerProducer + i;
                while (!q.enqueue(value).has_value()) {
                    // backpressure: spin retry
                    std::this_thread::yield();
                }
            }
        });
    }

    std::set<int> received;
    int total = kNumProducers * kItemsPerProducer;
    while (static_cast<int>(received.size()) < total) {
        if (auto item = q.dequeue(); item.has_value()) {
            received.insert(*item);
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : producers) t.join();

    // All items received, no duplicates
    EXPECT_EQ(static_cast<int>(received.size()), total);
}
```

**Step 2: Enable test in CMakeLists.txt**

`core/tests/unit/CMakeLists.txt`에서 해당 라인 uncomment:
```cmake
apex_add_unit_test(test_mpsc_queue test_mpsc_queue.cpp)
```

**Step 3: Run test to verify it fails**

```bash
cmake --preset debug && cmake --build build/debug
ctest --preset debug --tests-regex test_mpsc_queue -V
```

Expected: FAIL (링커 에러 또는 구현 없음)

**Step 4: Implement MpscQueue in header**

`core/include/apex/core/mpsc_queue.hpp` 하단에 구현 추가 (template이므로 header-only):

```cpp
// --- Implementation ---

namespace apex::core {

namespace detail {
    constexpr size_t next_power_of_2(size_t v) {
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16; v |= v >> 32;
        return v + 1;
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::MpscQueue(size_t capacity)
    : capacity_(detail::next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
{
    slots_ = new Slot[capacity_];
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
MpscQueue<T>::~MpscQueue() {
    delete[] slots_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::expected<void, QueueError> MpscQueue<T>::enqueue(const T& item) {
    size_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
        size_t tail = tail_;  // relaxed read is OK, may be stale (conservative)
        // Actually, tail_ is not atomic and only consumer reads it.
        // We need a separate atomic for producers to read.
        // Let's use a different approach: slot-based readiness.

        Slot& slot = slots_[head & mask_];
        if (slot.ready.load(std::memory_order_acquire)) {
            // Slot not yet consumed — queue is full
            // But head may have advanced, retry
            size_t new_head = head_.load(std::memory_order_relaxed);
            if (new_head == head) {
                return std::unexpected(QueueError::Full);
            }
            head = new_head;
            continue;
        }

        if (head_.compare_exchange_weak(head, head + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            slot.data = item;
            slot.ready.store(true, std::memory_order_release);
            return {};
        }
        // CAS failed, head updated by compare_exchange_weak, retry
    }
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
std::optional<T> MpscQueue<T>::dequeue() {
    Slot& slot = slots_[tail_ & mask_];
    if (!slot.ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    T item = slot.data;
    slot.ready.store(false, std::memory_order_release);
    ++tail_;
    return item;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::size_approx() const noexcept {
    size_t head = head_.load(std::memory_order_relaxed);
    // tail_ is consumer-only, reading from another thread is a data race.
    // For approx size, we count ready slots.
    size_t count = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        if (slots_[i].ready.load(std::memory_order_relaxed)) ++count;
    }
    return count;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t MpscQueue<T>::capacity() const noexcept {
    return capacity_;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
bool MpscQueue<T>::empty() const noexcept {
    return size_approx() == 0;
}

} // namespace apex::core
```

**Step 5: Run tests**

```bash
cmake --build build/debug && ctest --preset debug --tests-regex test_mpsc_queue -V
```

Expected: ALL PASS

**Step 6: Commit**

```bash
git add core/include/apex/core/mpsc_queue.hpp core/tests/unit/test_mpsc_queue.cpp core/tests/unit/CMakeLists.txt
git commit -m "feat: Phase 2A - MPSC lock-free bounded queue with tests"
```

---

### Task 2B: Slab Memory Pool (Agent B)

**Files:**
- Create: `core/src/slab_pool.cpp`
- Create: `core/tests/unit/test_slab_pool.cpp`
- Modify: `core/tests/unit/CMakeLists.txt`
- Modify: `core/CMakeLists.txt` (add src)

**Step 1: Write failing tests**

```cpp
// core/tests/unit/test_slab_pool.cpp
#include <apex/core/slab_pool.hpp>
#include <gtest/gtest.h>
#include <vector>
#include <set>

using namespace apex::core;

TEST(SlabPool, Construction) {
    SlabPool pool(64, 100);
    EXPECT_EQ(pool.total_count(), 100u);
    EXPECT_EQ(pool.free_count(), 100u);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_GE(pool.slot_size(), 64u);
}

TEST(SlabPool, AllocateAndDeallocate) {
    SlabPool pool(32, 10);
    void* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.allocated_count(), 1u);
    EXPECT_EQ(pool.free_count(), 9u);

    pool.deallocate(p);
    EXPECT_EQ(pool.allocated_count(), 0u);
    EXPECT_EQ(pool.free_count(), 10u);
}

TEST(SlabPool, AllocateAll) {
    SlabPool pool(16, 5);
    std::vector<void*> ptrs;
    for (int i = 0; i < 5; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.free_count(), 0u);

    // Pool exhausted
    EXPECT_EQ(pool.allocate(), nullptr);

    // Deallocate all
    for (void* p : ptrs) pool.deallocate(p);
    EXPECT_EQ(pool.free_count(), 5u);
}

TEST(SlabPool, NoDuplicatePointers) {
    SlabPool pool(64, 100);
    std::set<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(ptrs.insert(p).second) << "Duplicate pointer returned";
    }
}

TEST(SlabPool, ReuseAfterDeallocate) {
    SlabPool pool(32, 2);
    void* p1 = pool.allocate();
    pool.deallocate(p1);
    void* p2 = pool.allocate();
    EXPECT_EQ(p1, p2);  // LIFO free-list reuses same slot
}

// --- TypedSlabPool ---

struct TestObj {
    int x;
    float y;
    TestObj(int x, float y) : x(x), y(y) {}
};

TEST(TypedSlabPool, ConstructAndDestroy) {
    TypedSlabPool<TestObj> pool(10);
    TestObj* obj = pool.construct(42, 3.14f);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->x, 42);
    EXPECT_FLOAT_EQ(obj->y, 3.14f);
    EXPECT_EQ(pool.allocated_count(), 1u);

    pool.destroy(obj);
    EXPECT_EQ(pool.allocated_count(), 0u);
}
```

**Step 2: Enable test and add source in CMake**

`core/CMakeLists.txt`를 수정하여 src 추가:
```cmake
add_library(apex_core STATIC
    src/slab_pool.cpp
)
```

`core/tests/unit/CMakeLists.txt`:
```cmake
apex_add_unit_test(test_slab_pool test_slab_pool.cpp)
```

**Step 3: Run test to verify it fails**

```bash
cmake --preset debug && cmake --build build/debug
```

Expected: FAIL (slab_pool.cpp 없음)

**Step 4: Implement SlabPool**

```cpp
// core/src/slab_pool.cpp
#include <apex/core/slab_pool.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace apex::core {

static size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

SlabPool::SlabPool(size_t slot_size, size_t initial_count)
    : chunk_(nullptr)
    , free_list_(nullptr)
    , slot_size_(align_up(std::max(slot_size, sizeof(FreeNode)), alignof(std::max_align_t)))
    , total_count_(0)
    , free_count_(0)
{
    if (initial_count == 0) {
        throw std::invalid_argument("SlabPool: initial_count must be > 0");
    }
    grow(initial_count);
}

SlabPool::~SlabPool() {
    std::free(chunk_);
}

void SlabPool::grow(size_t count) {
    chunk_ = static_cast<uint8_t*>(std::aligned_alloc(64, slot_size_ * count));
    if (!chunk_) {
        throw std::bad_alloc();
    }

    // Build free-list from back to front (so first allocate returns first slot)
    for (size_t i = count; i > 0; --i) {
        auto* node = reinterpret_cast<FreeNode*>(chunk_ + (i - 1) * slot_size_);
        node->next = free_list_;
        free_list_ = node;
    }

    total_count_ += count;
    free_count_ += count;
}

void* SlabPool::allocate() {
    if (!free_list_) return nullptr;

    FreeNode* node = free_list_;
    free_list_ = node->next;
    --free_count_;
    return static_cast<void*>(node);
}

void SlabPool::deallocate(void* ptr) noexcept {
    if (!ptr) return;
    auto* node = static_cast<FreeNode*>(ptr);
    node->next = free_list_;
    free_list_ = node;
    ++free_count_;
}

size_t SlabPool::allocated_count() const noexcept {
    return total_count_ - free_count_;
}

size_t SlabPool::free_count() const noexcept {
    return free_count_;
}

size_t SlabPool::total_count() const noexcept {
    return total_count_;
}

size_t SlabPool::slot_size() const noexcept {
    return slot_size_;
}

} // namespace apex::core
```

**Step 5: Run tests**

```bash
cmake --build build/debug && ctest --preset debug --tests-regex test_slab_pool -V
```

Expected: ALL PASS

**Step 6: Commit**

```bash
git add core/src/slab_pool.cpp core/tests/unit/test_slab_pool.cpp core/tests/unit/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat: Phase 2B - Slab memory pool with typed wrapper and tests"
```

---

### Task 2C: Ring Buffer (Agent C)

**Files:**
- Create: `core/src/ring_buffer.cpp`
- Create: `core/tests/unit/test_ring_buffer.cpp`
- Modify: `core/tests/unit/CMakeLists.txt`
- Modify: `core/CMakeLists.txt` (add ring_buffer.cpp)

**Step 1: Write failing tests**

```cpp
// core/tests/unit/test_ring_buffer.cpp
#include <apex/core/ring_buffer.hpp>
#include <gtest/gtest.h>
#include <cstring>

using namespace apex::core;

TEST(RingBuffer, Construction) {
    RingBuffer rb(1024);
    EXPECT_EQ(rb.capacity(), 1024u);
    EXPECT_EQ(rb.readable_size(), 0u);
    EXPECT_EQ(rb.writable_size(), 1024u);
}

TEST(RingBuffer, CapacityRoundsUp) {
    RingBuffer rb(1000);
    EXPECT_EQ(rb.capacity(), 1024u);
}

TEST(RingBuffer, WriteAndRead) {
    RingBuffer rb(64);
    const uint8_t data[] = {1, 2, 3, 4, 5};

    auto w = rb.writable();
    ASSERT_GE(w.size(), sizeof(data));
    std::memcpy(w.data(), data, sizeof(data));
    rb.commit_write(sizeof(data));

    EXPECT_EQ(rb.readable_size(), 5u);

    auto r = rb.contiguous_read();
    ASSERT_GE(r.size(), 5u);
    EXPECT_EQ(r[0], 1);
    EXPECT_EQ(r[4], 5);

    rb.consume(5);
    EXPECT_EQ(rb.readable_size(), 0u);
}

TEST(RingBuffer, WrapAround) {
    RingBuffer rb(8);  // 8 bytes
    uint8_t data4[4] = {10, 20, 30, 40};

    // Write 6 bytes, consume 6, write 6 more → wrap around
    auto w = rb.writable();
    std::memcpy(w.data(), data4, 4);
    rb.commit_write(4);
    rb.consume(4);  // read_pos=4, write_pos=4

    // Now write 6 bytes: 4 at end + 2 wrap to start
    uint8_t data6[6] = {1, 2, 3, 4, 5, 6};
    w = rb.writable();
    // writable may only return contiguous part (4 bytes at end)
    size_t first = std::min(w.size(), size_t(6));
    std::memcpy(w.data(), data6, first);
    rb.commit_write(first);

    if (first < 6) {
        w = rb.writable();
        std::memcpy(w.data(), data6 + first, 6 - first);
        rb.commit_write(6 - first);
    }

    EXPECT_EQ(rb.readable_size(), 6u);

    // contiguous_read may return less than 6
    auto r = rb.contiguous_read();
    EXPECT_LE(r.size(), 6u);
}

TEST(RingBuffer, Linearize_Contiguous) {
    RingBuffer rb(64);
    uint8_t data[] = {1, 2, 3, 4, 5};
    auto w = rb.writable();
    std::memcpy(w.data(), data, 5);
    rb.commit_write(5);

    auto span = rb.linearize(5);
    ASSERT_EQ(span.size(), 5u);
    EXPECT_EQ(span[0], 1);
    EXPECT_EQ(span[4], 5);
}

TEST(RingBuffer, Linearize_WrapAround) {
    RingBuffer rb(8);

    // Fill 6, consume 6 → positions at 6
    uint8_t fill[6] = {0};
    auto w = rb.writable();
    std::memcpy(w.data(), fill, 6);
    rb.commit_write(6);
    rb.consume(6);

    // Write 4 bytes → wraps around (pos 6,7,0,1)
    uint8_t data[4] = {10, 20, 30, 40};
    w = rb.writable();
    size_t first = std::min(w.size(), size_t(4));
    std::memcpy(w.data(), data, first);
    rb.commit_write(first);
    if (first < 4) {
        w = rb.writable();
        std::memcpy(w.data(), data + first, 4 - first);
        rb.commit_write(4 - first);
    }

    // linearize should return contiguous view of all 4 bytes
    auto span = rb.linearize(4);
    ASSERT_EQ(span.size(), 4u);
    EXPECT_EQ(span[0], 10);
    EXPECT_EQ(span[1], 20);
    EXPECT_EQ(span[2], 30);
    EXPECT_EQ(span[3], 40);
}

TEST(RingBuffer, Linearize_NotEnoughData) {
    RingBuffer rb(16);
    auto span = rb.linearize(5);
    EXPECT_TRUE(span.empty());
}

TEST(RingBuffer, Reset) {
    RingBuffer rb(16);
    uint8_t d[8] = {};
    auto w = rb.writable();
    std::memcpy(w.data(), d, 8);
    rb.commit_write(8);

    rb.reset();
    EXPECT_EQ(rb.readable_size(), 0u);
    EXPECT_EQ(rb.writable_size(), 16u);
}
```

**Step 2: Enable test, add source**

`core/CMakeLists.txt`: `src/ring_buffer.cpp` 추가
`core/tests/unit/CMakeLists.txt`: uncomment ring_buffer test

**Step 3: Implement RingBuffer**

```cpp
// core/src/ring_buffer.cpp
#include <apex/core/ring_buffer.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace apex::core {

static size_t next_power_of_2(size_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16; v |= v >> 32;
    return v + 1;
}

RingBuffer::RingBuffer(size_t capacity)
    : linear_buf_(nullptr)
    , capacity_(next_power_of_2(capacity < 1 ? 1 : capacity))
    , mask_(capacity_ - 1)
{
    buffer_ = static_cast<uint8_t*>(std::aligned_alloc(64, capacity_));
    if (!buffer_) throw std::bad_alloc();
}

RingBuffer::~RingBuffer() {
    std::free(buffer_);
    std::free(linear_buf_);
}

std::span<uint8_t> RingBuffer::writable() noexcept {
    size_t w = write_pos_ & mask_;
    size_t r = read_pos_ & mask_;
    size_t avail = writable_size();

    if (avail == 0) return {};

    // Contiguous writable area
    if (w >= r) {
        // Write from w to end of buffer (or to capacity if buffer is empty)
        size_t to_end = capacity_ - w;
        if (read_pos_ == write_pos_) to_end = capacity_ - w;  // empty: full space from w
        return {buffer_ + w, std::min(to_end, avail)};
    } else {
        // Write from w to r
        return {buffer_ + w, avail};
    }
}

void RingBuffer::commit_write(size_t n) noexcept {
    write_pos_ += n;
}

std::span<const uint8_t> RingBuffer::contiguous_read() const noexcept {
    size_t avail = readable_size();
    if (avail == 0) return {};

    size_t r = read_pos_ & mask_;
    size_t to_end = capacity_ - r;
    return {buffer_ + r, std::min(to_end, avail)};
}

size_t RingBuffer::readable_size() const noexcept {
    return write_pos_ - read_pos_;
}

void RingBuffer::consume(size_t n) noexcept {
    read_pos_ += n;
}

std::span<const uint8_t> RingBuffer::linearize(size_t n) {
    if (readable_size() < n) return {};

    // Check if contiguous
    size_t r = read_pos_ & mask_;
    size_t to_end = capacity_ - r;

    if (to_end >= n) {
        // Already contiguous — zero-copy
        return {buffer_ + r, n};
    }

    // Wrap-around — need to copy into linear buffer
    if (linear_buf_size_ < n) {
        std::free(linear_buf_);
        linear_buf_ = static_cast<uint8_t*>(std::malloc(n));
        linear_buf_size_ = n;
    }

    // Copy first part (to end of buffer)
    std::memcpy(linear_buf_, buffer_ + r, to_end);
    // Copy second part (from start of buffer)
    std::memcpy(linear_buf_ + to_end, buffer_, n - to_end);

    return {linear_buf_, n};
}

size_t RingBuffer::capacity() const noexcept {
    return capacity_;
}

size_t RingBuffer::writable_size() const noexcept {
    return capacity_ - readable_size();
}

void RingBuffer::reset() noexcept {
    read_pos_ = 0;
    write_pos_ = 0;
}

} // namespace apex::core
```

**Step 4: Run tests**

```bash
cmake --build build/debug && ctest --preset debug --tests-regex test_ring_buffer -V
```

Expected: ALL PASS

**Step 5: Commit**

```bash
git add core/src/ring_buffer.cpp core/tests/unit/test_ring_buffer.cpp core/tests/unit/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat: Phase 2C - Ring buffer with zero-copy linearize and tests"
```

---

### Task 2D: Timing Wheel (Agent D)

**Files:**
- Create: `core/src/timing_wheel.cpp`
- Create: `core/tests/unit/test_timing_wheel.cpp`
- Modify: `core/tests/unit/CMakeLists.txt`
- Modify: `core/CMakeLists.txt` (add timing_wheel.cpp)

**Step 1: Write failing tests**

```cpp
// core/tests/unit/test_timing_wheel.cpp
#include <apex/core/timing_wheel.hpp>
#include <gtest/gtest.h>
#include <set>

using namespace apex::core;

TEST(TimingWheel, Construction) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });
    EXPECT_EQ(tw.active_count(), 0u);
    EXPECT_EQ(tw.current_tick(), 0u);
}

TEST(TimingWheel, ScheduleAndExpire) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(3);  // expires at tick 3
    EXPECT_EQ(tw.active_count(), 1u);

    tw.tick(); // tick 1
    tw.tick(); // tick 2
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 3 — should fire
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, ScheduleAtTickZero) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    tw.schedule(0);  // expires immediately on next tick? or this tick?
    // Convention: 0 ticks = expires on the very next tick
    tw.tick();
    EXPECT_EQ(expired.size(), 1u);
}

TEST(TimingWheel, Cancel) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(2);
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    tw.tick();
    tw.tick();
    EXPECT_TRUE(expired.empty());  // cancelled, should not fire
}

TEST(TimingWheel, Reschedule) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(2);
    tw.tick(); // tick 1

    tw.reschedule(id, 3);  // now expires at tick 1 + 3 = tick 4

    tw.tick(); // tick 2 — original deadline, should NOT fire
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 3
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 4 — new deadline, should fire
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

TEST(TimingWheel, MultipleEntries_SameSlot) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id1 = tw.schedule(5);
    auto id2 = tw.schedule(5);
    auto id3 = tw.schedule(5);

    for (int i = 0; i < 5; ++i) tw.tick();

    EXPECT_EQ(expired.size(), 3u);
    EXPECT_TRUE(expired.contains(id1));
    EXPECT_TRUE(expired.contains(id2));
    EXPECT_TRUE(expired.contains(id3));
}

TEST(TimingWheel, WrapAround) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(8, [&](TimingWheel::EntryId id) { expired.insert(id); });

    // Advance past one full rotation
    for (int i = 0; i < 8; ++i) tw.tick();

    auto id = tw.schedule(3);  // tick 8 + 3 = 11
    tw.tick(); tw.tick(); tw.tick();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

TEST(TimingWheel, LargeNumberOfEntries) {
    size_t expire_count = 0;
    TimingWheel tw(256, [&](TimingWheel::EntryId) { ++expire_count; });

    for (int i = 1; i <= 1000; ++i) {
        tw.schedule(i % 200 + 1);
    }
    EXPECT_EQ(tw.active_count(), 1000u);

    for (int i = 0; i < 250; ++i) tw.tick();
    EXPECT_EQ(expire_count, 1000u);
    EXPECT_EQ(tw.active_count(), 0u);
}
```

**Step 2: Enable test, add source**

**Step 3: Implement TimingWheel**

(core/src/timing_wheel.cpp — 이중 연결 리스트 기반 슬롯, O(1) insert/remove/tick)

구현 핵심:
- `entries_` 벡터에 Entry 풀 관리 (id로 O(1) 조회)
- 각 슬롯은 이중 연결 리스트의 헤드
- tick(): 현재 슬롯 순회 → 콜백 호출 → entry 정리
- reschedule(): 기존 슬롯에서 제거 → 새 슬롯에 삽입

**Step 4: Run tests**

```bash
cmake --build build/debug && ctest --preset debug --tests-regex test_timing_wheel -V
```

Expected: ALL PASS

**Step 5: Commit**

```bash
git add core/src/timing_wheel.cpp core/tests/unit/test_timing_wheel.cpp core/tests/unit/CMakeLists.txt core/CMakeLists.txt
git commit -m "feat: Phase 2D - Timing wheel with O(1) schedule/cancel/reschedule and tests"
```

---

## Phase 2 완료 검증

### Task 2.5: 전체 테스트 + 체크포인트

**Step 1: Run all tests**

```bash
cmake --preset debug && cmake --build build/debug && ctest --preset debug -V
```

Expected: ALL 4 test suites PASS

**Step 2: Write Phase 2 checkpoint**

파일 `docs/progress/phase-2-complete.md`:
```markdown
# Phase 2 Complete

## 구현된 컴포넌트
1. **MpscQueue<T>** — 락프리 bounded MPSC 큐, 백프레셔 지원
2. **SlabPool / TypedSlabPool<T>** — O(1) 슬랩 메모리 풀
3. **RingBuffer** — zero-copy 수신 버퍼 (linearize 지원)
4. **TimingWheel** — O(1) 타임아웃 관리 (schedule/cancel/reschedule)

## 테스트 현황
- 단위 테스트: 전체 통과
- MPSC 큐 멀티스레드 테스트: 통과
- TSAN/ASAN: Phase 3.5 통합 시 실행 예정

## 다음: Phase 3 (코어 프레임워크)
- io_context-per-core 엔진
- ServiceBase<T> + route<T> 디스패치
- 와이어 프로토콜
```

**Step 3: Commit**

```bash
git add -A
git commit -m "chore: Phase 2 complete - all foundation components implemented and tested"
git tag v0.0.1-phase2
```
