# fix(core): Session refcount_ atomic 전환 — TSAN data race 수정

- **날짜**: 2026-03-21
- **PR**: #60
- **CI 참조**: #213 (linux-tsan 실패), #214 (main push 통과)

## 문제

CI #213 `linux-tsan`에서 `test_server_multicore` (`CountingServiceFixture.ServicePerCoreInstance`) abort:

```
session.cpp:19: void apex::core::intrusive_ptr_release(Session*):
Assertion `s->refcount_ > 0 && "Session refcount underflow"' failed.
```

## 원인

`Session::refcount_`가 non-atomic `uint32_t`로 선언되어 있었음.
Per-core 설계 가정이지만 shutdown 시 io_context 소멸 경로에서
cross-thread release가 발생 가능. `TSAN_OPTIONS=halt_on_error=1`
환경에서 data race가 실제 값 corruption으로 발현하여 어설션 실패.

## 수정 내용

| 파일 | 변경 |
|------|------|
| `session.hpp` | `uint32_t refcount_` → `std::atomic<uint32_t> refcount_` |
| `session.hpp` | `intrusive_ptr_add_ref`: `fetch_add(relaxed)` |
| `session.hpp` | `refcount()`: `load(relaxed)` |
| `session.cpp` | `intrusive_ptr_release`: `fetch_sub(acq_rel)` + `prev == 1` 패턴 |

## 성능 영향

없음. refcount 조작은 세션 생성/소멸 시에만 발생 (메시지 hot path 아님).
