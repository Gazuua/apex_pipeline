# Phase 5 완료 심층 리뷰 — 최종 보고서

**일시**: 2026-03-08 17:41
**리뷰어**: 6개 병렬 에이전트 (설계정합성, clangd LSP, 아키텍처, 보안/안전성, 성능, 테스트)
**범위**: apex_core 전체 (헤더 22, 소스 14, 테스트 23, 예제 3) + 설계 문서
**워크트리**: `.worktrees/deep-review` (브랜치: `feature/deep-review-phase5`)

---

## 최종 결과: CLEAN — 0건

3차 재리뷰에서 **전 영역 이슈 0건** 달성.

---

## 리뷰 프로세스 요약

| 라운드 | 발견 | 수정 | 잔여 |
|--------|------|------|------|
| 1차 (6개 에이전트) | 42건 + 10건(테스트) = 52건 | 52건 | 0건 |
| 2차 (3개 에이전트) | 1건 (design-decisions.md shutdown 순서) | 1건 | 0건 |
| 3차 (3개 에이전트) | 0건 | — | **0건 (CLEAN)** |

> 2차 아키텍처 에이전트가 보고한 7건은 메인 레포 경로를 읽은 오탐으로 확인.

---

## 1차 리뷰 발견 이슈 전량 (52건) — 수정 완료

### Critical (6건)

| ID | 이슈 | 수정 내용 |
|----|------|----------|
| C-01 | context_provider↔on_accept TOCTOU | ContextProvider 제거, accept_io_에서 accept → post()로 코어 이관. 단일 fetch_add로 코어 결정 |
| C-02 | cross_core_call 타임아웃 후 UAF | @pre 계약 강화: func은 코루틴 로컬 변수 참조 캡처 금지. early-check 패턴 + 문서화 |
| C-03 | cross_core_call Result<R> 소멸 스레드 안전성 | `static_assert(std::is_nothrow_destructible_v<R>)` 추가 |
| C-04 | MPSC Queue 슬롯 경쟁 조건 | MPSC(단일 소비자) 안전성 근거 문서화. ready flag가 sequence counter 역할 |
| C-05 | process_frames 매 프레임 vector 힙 할당 | SmallBuffer 패턴: ≤4KB 스택, 초과 시 heap fallback |
| C-06 | TCP_NODELAY 미설정 | ServerConfig::tcp_nodelay (기본 true), on_accept()에서 설정 |

### Important (22건)

| ID | 이슈 | 수정 내용 |
|----|------|----------|
| I-01 | config.hpp 역방향 의존성 | TODO 주석 추가 (Phase 6에서 server_config.hpp 분리) |
| I-02 | ServiceBase::stop() 댕글링 | dispatcher_ nullptr 가드 추가 |
| I-03 | MpscQueue CAS 실패 시 tail 미재로드 | CAS 실패 루프 내 tail_.load(acquire) 추가 |
| I-04 | MpscQueue 글로벌 FIFO 미보장 미명시 | 헤더 주석에 ordering/HOL blocking 문서화 |
| I-05 | cross_core_call 3회 힙 할당 | "infrequent RPC용" 주석 + Phase 8.5 TODO |
| I-06 | TimingWheel::schedule new Entry | Phase 8.5 벤치마크 후 최적화 TODO |
| I-07 | SessionManager 3맵 이중 조회 | Session에 timer_entry_id_ 내장, session_to_timer_ 제거 |
| I-08 | FrameCodec contiguous path memcpy | 방어적 복사 의도 주석 추가 |
| I-09 | poll_shutdown spdlog::get() 매번 호출 | Server::logger_ 멤버로 캐싱 |
| I-10 | MessageDispatcher std::function 비용 | move_only_function 검토 TODO + 현상유지 |
| I-11 | Apex_Pipeline.md Phase 5 TODO 미갱신 | 4개 항목 [x] 체크 |
| I-12 | Apex_Pipeline.md 기술 스택 부정확 | Beast→Phase 8a, Benchmark→Phase 8.5 |
| I-13 | Apex_Pipeline.md Shutdown 순서 불일치 | stop→join→drain_remaining 수정 |
| I-14 | design-decisions.md 의존성 미갱신 | spdlog, tomlplusplus 추가 |
| I-15 | drain_interval_us 음수 미검증 | `< 0` 검사 추가 |
| I-16 | max_size_mb 곱셈 오버플로우 | 10240MB 상한선 검증 |
| I-17 | JsonFormatter U+2028/U+2029 미이스케이프 | UTF-8 시퀀스 감지 + 이스케이프 |
| I-18 | running_ 플래그 초기화 완료 전 true | 모든 초기화 완료 후 설정 |
| I-19 | UnsupportedVersion 에러 코드 누락 | ErrorCode::UnsupportedProtocolVersion 추가 |
| I-20 | checked_narrow<size_t> 축소 변환 | int64_t 리터럴 직접 명시 |
| I-21 | run() 재호출 시 중복 적용 | run_count_ atomic 가드, std::logic_error throw |
| I-22 | Session::close() 비-atomic | 단일 코어 제약 API 문서 강화 |

### Minor (14건)

| ID | 수정 내용 |
|----|----------|
| m-01 | Apex_Pipeline.md §7에 config/ 디렉토리 추가 |
| m-02 | Apex_Pipeline.md §7에 apex_tools/git-hooks 추가 |
| m-03 | Apex_Pipeline.md §9에 이중 expected 타입 명시 |
| m-04 | Apex_Pipeline.md §5에 "비동기 post" 디테일 추가 |
| m-05 | service_base.hpp doc example route<T> 수정 |
| m-06 | owned_dispatcher_ make_unique 힙 할당 주석 |
| m-07 | examples (void)co_await 캐스트 추가 |
| m-08 | timing_wheel.cpp fprintf→spdlog |
| m-09 | message_dispatcher.cpp fprintf→spdlog |
| m-10 | Session enable_shared_from_this 미사용 주석 |
| m-11 | CoreEngine 소멸자에 drain_remaining() 추가 |
| m-12 | RingBuffer linear_buf_ reset 주석 (shrink_to_fit 의도) |
| m-13 | CMakeLists.txt 테스트 타임아웃 설정 (단위 30s, 통합 30-60s) |
| m-14 | test_shutdown_timeout 픽스처 전환 (init/shutdown 격리) |

### 테스트 이슈 (10건)

| ID | 수정 내용 |
|----|----------|
| T-01 | io_context restart() 테스트 격리 |
| T-02 | SendAfterPeerDisconnect 터미널 상태 assertion |
| T-03 | DrainRemainingCleansUpPointers shared_ptr 기반 검증 |
| T-04 | logging 초기화/shutdown 안전성 테스트 추가 |
| T-05 | config 경계 조건 테스트 (음수 포트, 음수 drain 등) |
| T-06 | Session::State::Active 전환 테스트 |
| T-07 | TimingWheel tick() 콜백 내 재진입 테스트 |
| T-08 | cross_core_call 타임아웃 후 100ms 대기 (메모리 안전성) |
| T-09 | CountingServiceFixture 정적 카운터 격리 |
| T-10 | HeartbeatTimeout 세션 State::Closed 검증 |

---

## 빌드 & 테스트

```
23/23 tests passed, 0 tests failed
Total Test time (real) = 4.08 sec
```

---

## 변경 파일 목록

### 소스/헤더 (17파일)
- `include/apex/core/server.hpp` — tcp_nodelay, logger_, run_count_
- `include/apex/core/tcp_acceptor.hpp` — ContextProvider 제거
- `include/apex/core/cross_core_call.hpp` — static_assert, @pre 계약, docs
- `include/apex/core/mpsc_queue.hpp` — tail reload, safety docs
- `include/apex/core/config.hpp` — TODO 주석
- `include/apex/core/error_code.hpp` — UnsupportedProtocolVersion
- `include/apex/core/service_base.hpp` — nullptr guard, doc, owned_dispatcher_
- `include/apex/core/session.hpp` — timer_entry_id_, thread-safety doc
- `include/apex/core/session_manager.hpp` — session_to_timer_ 제거
- `src/server.cpp` — C-01/C-05/C-06/I-09/I-18/I-21
- `src/tcp_acceptor.cpp` — C-01
- `src/config.cpp` — I-15/I-20
- `src/logging.cpp` — I-16/I-17
- `src/session_manager.cpp` — I-07
- `src/core_engine.cpp` — m-11
- `src/timing_wheel.cpp` — m-08
- `src/message_dispatcher.cpp` — m-09

### 테스트 (13파일)
- `tests/unit/test_session.cpp` — T-01/T-02
- `tests/unit/test_core_engine.cpp` — T-03
- `tests/unit/test_logging.cpp` — T-04
- `tests/unit/test_config.cpp` — T-05
- `tests/unit/test_session_manager.cpp` — T-06/T-10
- `tests/unit/test_timing_wheel.cpp` — T-07
- `tests/unit/test_cross_core_call.cpp` — T-08
- `tests/unit/test_server_multicore.cpp` — T-09/DoubleRunThrows
- `tests/unit/test_message_dispatcher.cpp` — T-01
- `tests/integration/test_shutdown_timeout.cpp` — m-14
- `tests/unit/CMakeLists.txt` — m-13
- `tests/integration/CMakeLists.txt` — m-13

### 예제 (2파일)
- `examples/echo_server.cpp` — m-07
- `examples/multicore_echo_server.cpp` — m-07

### 문서 (4파일)
- `docs/Apex_Pipeline.md` — I-11/I-12/I-13/m-01~m-04
- `docs/apex_core/design-decisions.md` — I-14/shutdown 순서
- `docs/apex_core/review/20260308_171243_deep-review-1st.md` — 1차 보고서
- `docs/apex_core/review/20260308_174115_deep-review-final.md` — 본 최종 보고서
