# Post-E2E 코드 리뷰 실행 설계서

**이슈**: BACKLOG #48
**브랜치**: `feature/48-post-e2e-code-review`
**작성일**: 2026-03-19
**선행 계획서**: `docs/apex_common/plans/20260318_144700_post-e2e-code-review.md`

---

## 1. 결정 사항

| 항목 | 결정 |
|------|------|
| 실행 방식 | 하이브리드 — 계획서 10개 관점을 4 서브에이전트 그룹으로 병렬 실행 + 메인 취합 |
| 수정 범위 | 공격적 — CRITICAL/MAJOR 즉시 수정, MINOR만 백로그 |
| 선행 조건 | #1 프레임워크 가이드 완료 후 최종 수정 대상 확정 (Safety 그룹은 선행 수정 가능) |

## 2. 리뷰 대상 규모

- **PR 범위**: #27~#37 (v0.5.2.0 ~ v0.5.5.1)
- **프로덕션 코드**: ~18K줄 (apex_core 5.7K + apex_shared 6.1K + apex_services 6.3K)
- **핵심 변경**: PR #37 (45파일, +4,505/-1,426줄) — 서비스 레이어 오버홀

## 3. 서브에이전트 그룹 구성

### 그룹 A: Core-API (관점 ①②③)

**관점**:
- ① 코어 인터페이스 단순화 기회
- ② 초기화 순서 의존성 제약
- ③ OOP / 유지보수성

**파일 포커스**:
- `apex_core/include/apex/core/service_base.hpp`
- `apex_core/include/apex/core/server.hpp`, `apex_core/src/server.cpp`
- `apex_core/include/apex/core/configure_context.hpp`
- `apex_core/include/apex/core/wire_context.hpp`
- `apex_core/include/apex/core/service_registry.hpp`
- 3개 서비스 `main.cpp`
- 3개 서비스 헤더 (`gateway_service.hpp`, `auth_service.hpp`, `chat_service.hpp`)

**사전 관측 검증**: P1 (post_init_callback 미마이그레이션), P2 (sleep(1s) 하드코딩), P3 (dynamic_cast 인덱스 접근)

### 그룹 B: Architecture (관점 ④⑤)

**관점**:
- ④ shared-nothing 원칙 준수 / 고성능 모듈 활용
- ⑤ 서비스 간 의존성

**파일 포커스**:
- 전 서비스 헤더 및 소스
- `apex_shared/lib/protocols/kafka/include/` (kafka_dispatch_bridge, envelope_builder)
- `apex_services/gateway/src/channel_session_map.cpp`
- `apex_shared/schemas/*.fbs` (FlatBuffers 스키마)
- `#include` 그래프 전체 (cross-service 참조 여부)

**사전 관측 검증**: P4 (ChannelSessionMap 코어 간 공유 동기화)

### 그룹 C: Safety (관점 ⑥⑦⑧)

**관점**:
- ⑥ 코루틴 lifetime 안전성
- ⑦ 에러 핸들링 일관성
- ⑧ Shutdown 경로 정합성

**파일 포커스**:
- `co_spawn` / `co_await` 사용처 전체
- 모든 핸들러 함수 (route<T>, kafka_route<T> 콜백)
- `server.cpp` shutdown 시퀀스
- 3개 서비스 `on_stop()` 구현
- 어댑터 `close()` / `drain()` 경로

**사전 관측 검증**: P5 (co_spawn(detached) + [&engine] 캡처), P6 (on_stop() 최소 구현)

**특이사항**: 가이드 도착 전에도 CRITICAL/MAJOR 발견 시 선행 수정 가능 (코드 사실 기반 판단)

### 그룹 D: Sweep (관점 ⑨⑩)

**관점**:
- ⑨ 매직넘버 / 하드코딩 설정
- ⑩ 에이전트 자율 판단 항목

**파일 포커스**:
- 전체 프로덕션 코드 스캔
- TOML 설정 파일과 코드 내 기본값 대조
- 네이밍, `#include`, 로깅 레벨, 테스트 갭

## 4. 산출물 형식

각 서브에이전트는 아래 형식으로 발견 사항을 보고한다:

```
### 발견 #{N}
- **등급**: CRITICAL / MAJOR / MINOR
- **관점**: ①~⑩
- **위치**: 파일:줄번호
- **현상**: 무엇이 문제인가
- **근거**: 왜 문제인가 (코드 인용)
- **권장 수정**: 어떻게 고칠 것인가
```

## 5. 실행 워크플로우

```
Phase 1: 리뷰 (가이드 도착 전)
├─ [병렬] Core-API 그룹 (관점 ①②③)
├─ [병렬] Architecture 그룹 (관점 ④⑤)
├─ [병렬] Safety 그룹 (관점 ⑥⑦⑧)
├─ [병렬] Sweep 그룹 (관점 ⑨⑩)
└─ 메인: 4그룹 결과 취합 → 중복 제거 → 리뷰 보고서 초안

Phase 2: Safety 선행 수정 (가이드 불필요)
├─ Safety 그룹 CRITICAL/MAJOR 발견 사항 즉시 수정
└─ 빌드 + 테스트 검증

Phase 3: 가이드 대조 (유저가 #1 가이드 전달 후)
├─ 리뷰 발견 사항 × 가이드 교차 검증
├─ false positive 제거
└─ 최종 수정 대상 확정 리스트 유저에게 보고

Phase 4: 본격 수정
├─ CRITICAL/MAJOR 순서대로 수정
├─ 빌드 + 테스트 검증
├─ 리뷰 보고서 최종본 작성
└─ MINOR 이슈 → 백로그 등록
```

## 6. 최종 산출물

| 산출물 | 경로 | 시점 |
|--------|------|------|
| 리뷰 보고서 | `docs/apex_common/review/YYYYMMDD_HHMMSS_post-e2e-review.md` | Phase 4 완료 후 |
| 백로그 갱신 | `docs/BACKLOG.md` — MINOR 이슈 추가 | Phase 4 |
| 코드 수정 | `feature/48-post-e2e-code-review` 브랜치 | Phase 2 + 4 |
| progress 문서 | `docs/apex_common/progress/YYYYMMDD_HHMMSS_post-e2e-review.md` | 머지 전 |

## 7. 주의사항

- 서브에이전트는 리뷰만 수행, 코드 수정 금지 — 메인이 취합 후 수정
- 기존 백로그(#1~#60)와 중복되는 이슈는 새로 추가하지 않고 기존 항목에 메모 보강
- ASAN/TSAN 통과 상태이므로 sanitizer 범위의 메모리 이슈는 저확률 — 타이밍/논리 이슈에 집중
- 빌드는 서브에이전트가 하지 않음 — 메인이 직접 수행
