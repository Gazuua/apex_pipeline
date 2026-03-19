# apex_core 프레임워크 가이드 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 에이전트가 단일 문서만 읽고 apex_core 위에 새 서비스를 올릴 수 있는 통합 레퍼런스 작성

**Architecture:** 단일 파일(`docs/apex_core/apex_core_guide.md`) 2레이어 구조. 레이어 1(§1-§9)은 태스크 기반 API 가이드, 레이어 2(§10)는 내부 아키텍처 요약 + ADR 포인터, §11은 실전 패턴 부록. 설계 결정 D1-D7은 "의도된 설계"로 기술 (코드 구현은 #48 에이전트 담당).

**Tech Stack:** C++23, Boost.Asio, FlatBuffers, spdlog, toml++

**Spec:** `docs/apex_core/plans/20260319_114516_apex-core-framework-guide-spec.md`

---

## 파일 맵

| 액션 | 파일 | 역할 |
|------|------|------|
| **Create** | `docs/apex_core/apex_core_guide.md` | 프레임워크 가이드 본체 |
| **Modify** | `CLAUDE.md` | 유지보수 규칙 + 가이드 포인터 추가 |
| **Modify** | `docs/BACKLOG.md` | #1 완료 삭제 + 스코프 외 항목 등록 |
| **Modify** | `docs/BACKLOG_HISTORY.md` | #1 완료 기록 |

---

## 참조 소스 (태스크별 Read 대상)

| 약어 | 경로 |
|------|------|
| **SRV** | `apex_core/include/apex/core/server.hpp` |
| **SB** | `apex_core/include/apex/core/service_base.hpp` |
| **CC** | `apex_core/include/apex/core/configure_context.hpp` |
| **WC** | `apex_core/include/apex/core/wire_context.hpp` |
| **WH** | `apex_core/include/apex/core/wire_header.hpp` 또는 `apex_shared/` 내 실제 위치 |
| **BUMP** | `apex_core/include/apex/core/bump_allocator.hpp` |
| **ARENA** | `apex_core/include/apex/core/arena_allocator.hpp` |
| **XCC** | `apex_core/include/apex/core/cross_core_call.hpp` |
| **PTS** | `apex_core/include/apex/core/periodic_task_scheduler.hpp` |
| **SESS** | `apex_core/include/apex/core/session.hpp` |
| **RES** | `apex_core/include/apex/core/result.hpp` |
| **PROTO** | `apex_core/include/apex/core/protocol.hpp` |
| **CFG** | `apex_core/include/apex/core/config.hpp` |
| **LOG** | `apex_core/include/apex/core/logging.hpp` |
| **CE** | `apex_core/include/apex/core/core_engine.hpp` |
| **CH** | `apex_core/include/apex/core/connection_handler.hpp` |
| **MD** | `apex_core/include/apex/core/message_dispatcher.hpp` |
| **SR** | `apex_core/include/apex/core/service_registry.hpp` |
| **SRV_CPP** | `apex_core/src/server.cpp` |
| **GW** | `apex_services/gateway/src/gateway_service.cpp` |
| **GW_H** | `apex_services/gateway/include/apex/gateway/gateway_service.hpp` |
| **GW_MAIN** | `apex_services/gateway/src/main.cpp` |
| **AUTH** | `apex_services/auth-svc/src/auth_service.cpp` |
| **AUTH_MAIN** | `apex_services/auth-svc/src/main.cpp` |
| **CHAT** | `apex_services/chat-svc/src/chat_service.cpp` |
| **CHAT_MAIN** | `apex_services/chat-svc/src/main.cpp` |
| **AUTH_CMAKE** | `apex_services/auth-svc/CMakeLists.txt` |
| **DD** | `docs/apex_core/design-decisions.md` |
| **DR** | `docs/apex_core/design-rationale.md` |
| **HANDOFF** | (외부) `post-e2e-review-handoff.md` — 핸드오프 문서 내용은 스펙 §3 D1-D7에 이미 반영됨 |

---

### Task 1: §1 퀵 레퍼런스 — 서비스 스켈레톤

**Files:**
- Create: `docs/apex_core/apex_core_guide.md`
- Read: SB, SRV, AUTH_MAIN, CFG, LOG

**목표:** 문서 헤더 + 목차 + 최소 동작 서비스 전체 코드 (MyService 클래스 + main())

- [ ] **Step 1:** Read SB, SRV — ServiceBase CRTP 시그니처, Server fluent API 확인
- [ ] **Step 2:** Read AUTH_MAIN — 실제 main() 패턴 (TOML, 어댑터, 로깅) 확인
- [ ] **Step 3:** Read CFG, LOG — AppConfig, LogConfig, init_logging 시그니처 확인
- [ ] **Step 4:** `docs/apex_core/apex_core_guide.md` 생성 — 문서 헤더(버전, 갱신일, 목적), 전체 목차(§1-§11 앵커), §1 스켈레톤 코드 작성. 스켈레톤은 TCP + Kafka 양쪽 예시 포함. D2 반영: Kafka 서비스는 `kafka_route<T>()` 등록만으로 자동 배선.
- [ ] **Step 5:** Commit `docs(guide): #1 §1 퀵 레퍼런스 — 서비스 스켈레톤`

---

### Task 2: §2 Server 설정 & 부트스트랩

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: SRV, CFG, LOG, AUTH_MAIN, GW_MAIN, CHAT_MAIN, `apex_core/config/default.toml`, `apex_services/auth-svc/auth_svc.toml`

- [ ] **Step 1:** Read SRV — ServerConfig 전체 필드 + 기본값 추출
- [ ] **Step 2:** Read AUTH_MAIN, GW_MAIN — add_service vs add_service_factory 사용 패턴, TOML 파싱 패턴 비교
- [ ] **Step 3:** Read CFG, LOG, default.toml — LogConfig 필드, TOML 매핑
- [ ] **Step 4:** §2 작성 — §2.1 ServerConfig 테이블, §2.2 Server fluent API (`server.global<T>(factory)` D3 포함), §2.3 TOML + LogConfig + init_logging (#60), §2.4 Kafka 자동 배선 (D2)
- [ ] **Step 5:** Commit `docs(guide): #1 §2 Server 설정 & 부트스트랩`

---

### Task 3: §3 라이프사이클 훅

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: SB, SRV_CPP, CC, WC, SR, GW, AUTH, CHAT

- [ ] **Step 1:** Read SRV_CPP — Phase 1→2→3→3.5 오케스트레이션 코드 추적
- [ ] **Step 2:** Read CC, WC, SR — 각 Context 필드, ServiceRegistry API 확인
- [ ] **Step 3:** Read GW — on_session_closed, on_stop 패턴 확인
- [ ] **Step 4:** §3 작성 — Phase 테이블 (제공되는 것 / 아직 없는 것 / 전형적 용도 / 금지사항), D1 반영 (`registry.get<T>()` 정석), 런타임 훅 (on_session_closed), Shutdown 시퀀스 (D7 outstanding 코루틴 카운터 대기)
- [ ] **Step 5:** Commit `docs(guide): #1 §3 라이프사이클 훅`

---

### Task 4: §4 핸들러 & 메시지

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: SB, WH, MD, GW, AUTH, CHAT, `apex_shared/schemas/` 내 .fbs 파일들

- [ ] **Step 1:** Read SB — handle(), route<T>(), kafka_route<T>(), set_default_handler() 시그니처 추출
- [ ] **Step 2:** Read WH — WireHeader 필드 확인
- [ ] **Step 3:** Read `apex_shared/schemas/` — .fbs 파일 구조, msg_id 범위 패턴 확인
- [ ] **Step 4:** Read GW, AUTH — 실제 핸들러 예시 + send_response 패턴 + ErrorSender 사용
- [ ] **Step 5:** §4 작성 — §4.1 핸들러 4종 (시그니처 + 예시), §4.2 메시지 정의 (msg_id 규약, .fbs 배치), §4.3 와이어 프로토콜 (WireHeader, 응답 API 선택, ErrorSender, 에러 응답 헬퍼 패턴 D5)
- [ ] **Step 6:** Commit `docs(guide): #1 §4 핸들러 & 메시지`

---

### Task 5: §5 어댑터 접근 + §6 메모리 관리

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: SRV, BUMP, ARENA, AUTH, CHAT, GW, `apex_shared/adapters/` 내 config 헤더들

- [ ] **Step 1:** Read 어댑터 config 헤더들 — KafkaConfig, RedisConfig, PgAdapterConfig 필드
- [ ] **Step 2:** Read AUTH, CHAT, GW — on_configure() 어댑터 획득 패턴, role 기반 다중 인스턴스
- [ ] **Step 3:** Read BUMP, ARENA — API, capacity 설정
- [ ] **Step 4:** §5 작성 — 어댑터 획득 패턴, 3종 어댑터, role 기반, 에러 처리 (CircuitOpen, PoolExhausted)
- [ ] **Step 5:** §6 작성 — bump/arena API + 용도 판단 기준 + Kafka consumer 메모리 풀 (D6)
- [ ] **Step 6:** Commit `docs(guide): #1 §5 어댑터 접근 + §6 메모리 관리`

---

### Task 6: §7 유틸리티 + §8 금지사항

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: XCC, PTS, SESS, RES, DD, GW, AUTH, CHAT

- [ ] **Step 1:** Read XCC, PTS, SESS, RES — API 시그니처 추출
- [ ] **Step 2:** Read DD — shared-nothing ADR, 코루틴 lifetime ADR 확인
- [ ] **Step 3:** Read GW, AUTH — 실제 안티패턴 발생 지점 / co_await 후 포인터 예시 확인
- [ ] **Step 4:** §7 작성 — cross_core_call/post (D4 per-core 복제 패턴), PeriodicTaskScheduler, Session API, Result<T>, spawn() tracked API (D7)
- [ ] **Step 5:** §8 작성 — 7종 안티패턴 BAD/GOOD 코드 쌍. 각 항목: 코드 예시 + 1-2줄 설명 + 위험
- [ ] **Step 6:** Commit `docs(guide): #1 §7 유틸리티 + §8 금지사항`

---

### Task 7: §9 빌드 시스템 통합

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: AUTH_CMAKE, `apex_services/CMakeLists.txt`, `apex_services/auth-svc/schemas/`

- [ ] **Step 1:** Read AUTH_CMAKE — CMakeLists.txt 구조, FlatBuffers 컴파일, 링크 타겟 확인
- [ ] **Step 2:** Read `apex_services/CMakeLists.txt` — add_subdirectory 패턴 확인
- [ ] **Step 3:** §9 작성 — 디렉토리 구조 템플릿, CMakeLists.txt 템플릿 (FlatBuffers 커스텀 커맨드 포함), 필수 link_libraries 목록, add_subdirectory 위치
- [ ] **Step 4:** Commit `docs(guide): #1 §9 빌드 시스템 통합`

---

### Task 8: §10 내부 아키텍처 + §10.5 ADR 포인터

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: SRV, SRV_CPP, CE, CH, MD, PROTO, DD, DR

- [ ] **Step 1:** Read SRV, CE — Server/CoreEngine/PerCoreState 구조 확인
- [ ] **Step 2:** Read CH, MD, PROTO — 요청 처리 흐름 (try_decode → dispatch → handler) 추적
- [ ] **Step 3:** Read DD, DR — 서비스 개발 관련 ADR 10개 내외 선별
- [ ] **Step 4:** §10 작성 — §10.1 컴포넌트 배치도 (텍스트 다이어그램, GlobalResourceRegistry 포함), §10.2 Phase 시퀀스, §10.3 TCP 요청 흐름, §10.4 Kafka 메시지 흐름
- [ ] **Step 5:** §10.5 ADR 포인터 테이블 작성 — 주제 | ADR 번호 | 파일 경로
- [ ] **Step 6:** Commit `docs(guide): #1 §10 내부 아키텍처 + ADR 포인터`

---

### Task 9: §11 실전 서비스 패턴

**Files:**
- Modify: `docs/apex_core/apex_core_guide.md`
- Read: GW, GW_H, GW_MAIN, AUTH, AUTH_MAIN, CHAT, CHAT_MAIN

- [ ] **Step 1:** Read GW, GW_H — Gateway 패턴 핵심 코드 추출 (default_handler, per-session, globals)
- [ ] **Step 2:** Read AUTH, AUTH_MAIN — Kafka-only 패턴 핵심 코드 추출
- [ ] **Step 3:** Read CHAT — 어댑터 다중 역할 + 응답 전송 패턴 추출
- [ ] **Step 4:** §11 작성 — 4가지 패턴 각각: 상황 + 핵심 코드 40-60줄 + 10줄 해설. D2(Kafka 자동 배선), D3(server.global<T>()), D5(에러 응답 자체 헬퍼) 반영
- [ ] **Step 5:** Commit `docs(guide): #1 §11 실전 서비스 패턴`

---

### Task 10: CLAUDE.md 갱신 + 백로그 정리 + 최종 커밋

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/BACKLOG.md`
- Modify: `docs/BACKLOG_HISTORY.md`

- [ ] **Step 1:** `CLAUDE.md` — 가이드 유지보수 규칙 추가 (갱신 트리거, 갱신 범위, 머지 전 체크) + 가이드 포인터 테이블에 행 추가
- [ ] **Step 2:** `docs/BACKLOG.md` — #1 항목 삭제. 스코프 외 항목 등록: CLAUDE.md 중복 정리, 서비스 테스트 가이드, auto-review 가이드 검증 자동화
- [ ] **Step 3:** `docs/BACKLOG_HISTORY.md` — #1 완료 기록 (DOCUMENTED)
- [ ] **Step 4:** Commit `docs: #1 CLAUDE.md 유지보수 규칙 + 백로그 정리`
- [ ] **Step 5:** 전체 가이드 최종 검토 — 목차 앵커 정합성, 섹션 간 참조, D1-D7 반영 여부 확인
