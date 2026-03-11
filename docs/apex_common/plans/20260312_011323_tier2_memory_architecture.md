# Phase 5.5 Tier 2: 메모리 아키텍처 개선 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** SessionPtr을 shared_ptr에서 intrusive_ptr(non-atomic refcount)로 전환하고, Session 할당을 per-core SlabPool로 이전하며, SessionManager 내부 컨테이너를 boost::unordered_flat_map으로 교체하여 메모리 오버헤드와 캐시 오염을 최소화한다.

**Architecture:** 3단계 전환 — (1) intrusive_ptr typedef 변경 + refcount 멤버 추가 + 안전망, (2) SlabPool 연동 (create_session → pool.construct + heap fallback), (3) unordered_map → flat_map 교체. SessionPtr typedef 덕분에 서비스 코드 변경 최소화.

**Tech Stack:** C++23, boost::intrusive_ptr, per-core SlabPool (기 구현), boost::unordered_flat_map

**v6 계획서 참조**: `docs/apex_common/plans/20260311_204613_phase5_5_v6.md` § 2.1~2.3, § 6 Tier 2

**선행 조건**: Tier 1.5 완료 (E2E 부하 테스터로 baseline 측정 완료)

---

## File Structure

### Modified Files

| File | Change |
|------|--------|
| `apex_core/include/apex/core/session.hpp` | `enable_shared_from_this` 제거, `refcount_` 멤버 추가, `intrusive_ptr_add_ref`/`intrusive_ptr_release` friend 함수, SessionPtr typedef → `boost::intrusive_ptr<Session>` |
| `apex_core/src/session.cpp` | `intrusive_ptr_release` 구현 (pool.owns → pool.destroy, else delete) |
| `apex_core/include/apex/core/session_manager.hpp` | `sessions_`/`timer_to_session_` → `boost::unordered_flat_map`, `session_pool_` 멤버 추가, `create_session` SlabPool 연동 |
| `apex_core/src/session_manager.cpp` | SlabPool 초기화, create_session 변경, remove_session 변경 |
| `apex_core/include/apex/core/server.hpp` | PerCoreState에 SlabPool 참조 전달 (필요 시) |
| `apex_core/src/server.cpp` | PerCoreState 초기화 시 SlabPool 생성/전달 |
| `apex_core/tests/unit/test_session.cpp` | `make_shared<Session>` → 직접 `new Session` + `intrusive_ptr`, refcount 테스트 추가 |
| `apex_core/tests/unit/test_session_manager.cpp` | flat_map + SlabPool 기반 동작 검증 |
| `apex_core/tests/unit/test_service_base.cpp` | SessionPtr 생성 방식 변경 (make_shared → intrusive_ptr) |
| `apex_core/tests/unit/test_flatbuffers_dispatch.cpp` | 동일 |
| `apex_core/tests/integration/test_pipeline_integration.cpp` | 동일 |
| `apex_core/tests/integration/test_server_e2e.cpp` | E2E 동작 확인 |

---

## Chunk 1: intrusive_ptr 전환 (Tasks 1–2)

### Task 1: Session에 intrusive_ptr 인프라 추가 + SessionPtr typedef 변경

**Files:**
- Modify: `apex_core/include/apex/core/session.hpp`
- Modify: `apex_core/src/session.cpp`
- Test: `apex_core/tests/unit/test_session.cpp`

- [ ] **Step 1: test_session.cpp — intrusive_ptr refcount 테스트 추가**

```cpp
TEST(SessionTest, IntrusiveRefcount) {
    // intrusive_ptr 기본 동작 확인
    auto io = boost::asio::io_context{};
    tcp::socket sock(io);
    auto* raw = new Session(1, std::move(sock), 0, 8192);

    // 초기 refcount = 0
    EXPECT_EQ(raw->refcount(), 0u);

    {
        SessionPtr p1(raw);  // refcount → 1
        EXPECT_EQ(raw->refcount(), 1u);

        SessionPtr p2 = p1;  // refcount → 2
        EXPECT_EQ(raw->refcount(), 2u);
    }
    // p1, p2 소멸 → refcount 0 → delete (ASAN으로 leak 검증)
}

TEST(SessionTest, IntrusiveMoveSemantics) {
    auto io = boost::asio::io_context{};
    tcp::socket sock(io);
    auto* raw = new Session(1, std::move(sock), 0, 8192);

    SessionPtr p1(raw);
    EXPECT_EQ(raw->refcount(), 1u);

    SessionPtr p2 = std::move(p1);
    EXPECT_EQ(p1.get(), nullptr);
    EXPECT_EQ(raw->refcount(), 1u);
}
```

- [ ] **Step 2: 테스트 실패 확인**

Expected: FAIL — SessionPtr이 아직 shared_ptr이므로 `refcount()` 미존재

- [ ] **Step 3: session.hpp — intrusive_ptr 전환**

```cpp
#pragma once

#include <boost/intrusive_ptr.hpp>

// ... 기존 includes ...

namespace apex::core {

class Session {
public:
    // 기존 생성자/소멸자 유지
    Session(SessionId id, tcp::socket socket, uint32_t core_id, size_t recv_buf_capacity);
    ~Session();

    // enable_shared_from_this 제거

    // --- intrusive_ptr refcount ---
    // non-atomic: per-core 전용, 코어 경계 넘기지 않음
    [[nodiscard]] uint32_t refcount() const noexcept { return refcount_; }

    friend void intrusive_ptr_add_ref(Session* s) noexcept {
        assert(s->refcount_ < UINT32_MAX && "Session refcount overflow");
        ++s->refcount_;
    }

    friend void intrusive_ptr_release(Session* s) noexcept;

    // ... 기존 public API 유지 ...

private:
    uint32_t refcount_{0};  // non-atomic refcount
    // ... 기존 멤버 유지 ...
};

using SessionPtr = boost::intrusive_ptr<Session>;

} // namespace apex::core
```

> **핵심**: `enable_shared_from_this` 제거 (사용처 0건 확인), SessionPtr typedef만 변경하면 서비스 코드 영향 없음.
> **non-atomic 근거**: per-core 아키텍처에서 Session은 하나의 코어에 귀속. 코어 경계는 SessionId(uint64_t)로만 전달.

- [ ] **Step 4: session.cpp — intrusive_ptr_release 구현**

```cpp
void intrusive_ptr_release(Session* s) noexcept {
    assert(s->refcount_ > 0 && "Session refcount underflow");
    if (--s->refcount_ == 0) {
        delete s;  // Task 2에서 SlabPool 연동 시 pool.owns(s) 분기 추가
    }
}
```

- [ ] **Step 5: 기존 make_shared 제거 — 테스트 파일 갱신**

`test_session.cpp`에서 `std::make_shared<Session>(...)` → `SessionPtr(new Session(...))`로 변경.
`test_service_base.cpp`, `test_flatbuffers_dispatch.cpp`, `test_pipeline_integration.cpp`에서도 동일 변경.

> `make_shared`는 `shared_ptr` 전용. `intrusive_ptr`에서는 `new` + 생성자로 직접 생성.

- [ ] **Step 6: 전체 빌드 + 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS — SessionPtr typedef 변경만으로 서비스 코드 컴파일 유지

- [ ] **Step 7: TSAN 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat tsan" && ctest --preset tsan -V`
Expected: PASS — non-atomic refcount, 단일 코어 스레드에서만 접근

- [ ] **Step 8: 커밋**

```bash
git add apex_core/include/apex/core/session.hpp \
        apex_core/src/session.cpp \
        apex_core/tests/unit/test_session.cpp \
        apex_core/tests/unit/test_service_base.cpp \
        apex_core/tests/unit/test_flatbuffers_dispatch.cpp \
        apex_core/tests/integration/test_pipeline_integration.cpp
git commit -m "refactor(session): shared_ptr → intrusive_ptr (non-atomic refcount)"
```

---

### Task 2: Session SlabPool 전환

**Files:**
- Modify: `apex_core/include/apex/core/session_manager.hpp`
- Modify: `apex_core/src/session_manager.cpp`
- Modify: `apex_core/src/session.cpp` (intrusive_ptr_release에 pool 분기)
- Modify: `apex_core/include/apex/core/server.hpp` (PerCoreState에 pool 참조)
- Test: `apex_core/tests/unit/test_session_manager.cpp`

- [ ] **Step 1: test_session_manager.cpp — SlabPool 할당 테스트 추가**

```cpp
TEST_F(SessionManagerTest, CreateSessionUsesSlabPool) {
    auto session = mgr_.create_session(std::move(socket));
    ASSERT_NE(session, nullptr);
    // SlabPool에서 할당됨을 간접 확인 (pool exhaustion 후 heap fallback)
    EXPECT_GT(session->refcount(), 0u);
}
```

- [ ] **Step 2: session_manager.hpp — session_pool_ 멤버 추가**

```cpp
#include <apex/core/slab_pool.hpp>

class SessionManager {
    // ...
private:
    TypedSlabPool<Session> session_pool_;  // per-core Session 슬랩 풀
    // ...
};
```

- [ ] **Step 3: session_manager.cpp — create_session SlabPool 연동**

```cpp
SessionPtr SessionManager::create_session(tcp::socket socket) {
    auto id = next_id_++;
    Session* s = session_pool_.construct(id, std::move(socket), core_id_, recv_buf_capacity_);
    if (!s) {
        // SlabPool exhausted → heap fallback
        s = new Session(id, std::move(socket), core_id_, recv_buf_capacity_);
        if (logger_) {
            logger_->warn("Session SlabPool exhausted, falling back to heap (core {})", core_id_);
        }
    }
    return SessionPtr(s);
}
```

- [ ] **Step 4: session.cpp — intrusive_ptr_release에 pool 분기 추가**

```cpp
void intrusive_ptr_release(Session* s) noexcept {
    assert(s->refcount_ > 0 && "Session refcount underflow");
    if (--s->refcount_ == 0) {
        // pool 소속 판단은 SessionManager가 담당
        // Session에 pool 포인터를 저장하거나, SessionManager::destroy_session() 사용
        // 간단한 접근: Session에 pool_owner_ 포인터 저장
        if (s->pool_owner_) {
            s->pool_owner_->destroy(s);
        } else {
            delete s;
        }
    }
}
```

> **대안 설계**: Session 내부에 `TypedSlabPool<Session>* pool_owner_{nullptr}` 멤버를 두고, SlabPool에서 construct 시 설정. 이렇게 하면 intrusive_ptr_release가 자동으로 올바른 해제 경로를 선택.

- [ ] **Step 5: SessionManager 초기화 — pool 크기 계산**

```cpp
SessionManager::SessionManager(uint32_t core_id, /* ... */, uint32_t max_sessions_per_core)
    : session_pool_(max_sessions_per_core)  // 초기 용량
    // ...
{
}
```

> `max_sessions_per_core` = `(max_connections / num_cores) * 1.2` (v6 계획서 § 2.2)

- [ ] **Step 6: 전체 빌드 + 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS

- [ ] **Step 7: ASAN 테스트** (use-after-free, double-free 검증)

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat asan" && ctest --preset asan -V`
Expected: PASS

- [ ] **Step 8: 커밋**

```bash
git add apex_core/include/apex/core/session.hpp \
        apex_core/src/session.cpp \
        apex_core/include/apex/core/session_manager.hpp \
        apex_core/src/session_manager.cpp \
        apex_core/include/apex/core/server.hpp \
        apex_core/tests/unit/test_session_manager.cpp
git commit -m "feat(session): Session SlabPool 전환 — per-core 메모리 풀 할당"
```

---

## Chunk 2: 컨테이너 교체 (Task 3)

### Task 3: sessions_/timer_to_session_ → boost::unordered_flat_map

**Files:**
- Modify: `apex_core/include/apex/core/session_manager.hpp`
- Modify: `apex_core/src/session_manager.cpp`
- Test: `apex_core/tests/unit/test_session_manager.cpp`

- [ ] **Step 1: test_session_manager.cpp — 컨테이너 교체 후 동작 테스트**

기존 테스트가 모두 통과해야 함. 추가:
```cpp
TEST_F(SessionManagerTest, FlatMapLookupPerformance) {
    // 1000개 세션 생성 후 조회 성능 간접 확인
    // (실제 성능은 벤치마크에서 측정, 여기서는 기능 검증)
    for (int i = 0; i < 1000; ++i) {
        auto session = mgr_.create_session(/* ... */);
        EXPECT_NE(session, nullptr);
    }
    EXPECT_EQ(mgr_.session_count(), 1000u);
}
```

- [ ] **Step 2: session_manager.hpp — flat_map 교체**

```cpp
#include <boost/unordered/unordered_flat_map.hpp>

class SessionManager {
    // ...
private:
    boost::unordered_flat_map<SessionId, SessionPtr> sessions_;
    boost::unordered_flat_map<TimingWheel::EntryId, SessionId> timer_to_session_;
    // ...
};
```

> `std::unordered_map` → `boost::unordered_flat_map`: open-addressing, 인라인 저장. 캐시 친화도 향상.

- [ ] **Step 3: session_manager.cpp — API 변경 없음, 내부만 교체**

`insert`, `find`, `erase` 인터페이스 동일. 코드 변경 최소.

- [ ] **Step 4: 전체 빌드 + 테스트**

Run: `cmd.exe //c "D:\.workspace\apex_core\build.bat debug" && ctest --preset debug -V`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/session_manager.hpp \
        apex_core/src/session_manager.cpp \
        apex_core/tests/unit/test_session_manager.cpp
git commit -m "refactor(session): sessions_/timer_to_session_ → boost::unordered_flat_map"
```

---

## Summary

### 핵심 변경 3가지
1. **SessionPtr**: `shared_ptr<Session>` → `boost::intrusive_ptr<Session>` (non-atomic refcount)
2. **Session 할당**: `make_shared` (malloc) → per-core SlabPool (+ heap fallback)
3. **SessionManager 맵**: `std::unordered_map` → `boost::unordered_flat_map` (open-addressing)

### 아키텍처 불변조건
> **SessionPtr은 코어 경계를 넘지 않는다.** cross-core는 SessionId(uint64_t)로만 전달.

### Task 별 커밋 (3개)
1. `refactor(session): shared_ptr → intrusive_ptr (non-atomic refcount)`
2. `feat(session): Session SlabPool 전환 — per-core 메모리 풀 할당`
3. `refactor(session): sessions_/timer_to_session_ → boost::unordered_flat_map`
