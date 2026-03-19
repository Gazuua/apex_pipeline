# Post-E2E 코드 리뷰 + 프레임워크 인프라 정비 — Progress

**이슈**: BACKLOG #48
**버전**: v0.5.6.0
**브랜치**: `feature/48-post-e2e-code-review`
**작성일**: 2026-03-19

---

## 작업 결과 요약

v0.5.5.1 E2E 11/11 통과 이후 미수행된 코드 품질 리뷰를 10개 관점으로 수행하고, 프레임워크 가이드(#1)의 설계 결정 D2-D7을 구현하여 v0.6 진입 전 기틀을 정비했다.

### 수치 요약

| 항목 | 수치 |
|------|------|
| 리뷰 발견 이슈 (고유) | 46건 |
| 직접 수정 (Phase 2 + A + B + C) | ~35건 |
| Auto-review 추가 수정 | CRITICAL 4 + MAJOR 5건 |
| 변경 파일 | ~45개 |
| 신규 파일 | 4개 |
| 테스트 추가 | 6건 (D7 spawn) |
| 최종 테스트 | 71/71 전체 통과 |
| 잔여 이슈 → 백로그 | #66~#72 (7건) |

### Phase 별 성과

**Phase 1 — 4그룹 병렬 리뷰**
- Core-API (①②③): 12건 (MAJOR 9, MINOR 3)
- Architecture (④⑤): 9건 (MAJOR 4, MINOR 5)
- Safety (⑥⑦⑧): 11건 (CRITICAL 1, MAJOR 8, MINOR 2)
- Sweep (⑨⑩): 20건 (MAJOR 9, MINOR 11)

**Phase 2 — 즉시 수정 (25건)**
- Safety/shutdown 7건, 에러 핸들링 8건, 매직넘버/설정 외부화 10건
- 5개 모듈별 서브에이전트 병렬 투입 → 빌드 성공

**Phase A — 코어 인프라 확장**
- D3: `server.global<T>()` — cross-core 공유 자원 Server 소유
- D2: `AdapterInterface.wire_services()` — KafkaAdapter 자동 배선
- D7: `ServiceBase.spawn()` — outstanding 코루틴 추적 + shutdown 대기

**Phase B — 서비스 마이그레이션**
- D3 적용: GatewayGlobals → `server.global<GatewayGlobals>()` raw ptr 이관
- D2 적용: Auth/Chat `post_init_callback` 완전 제거, `sleep_for(1s)` 삭제
- D4: ChannelSessionMap per-core 전환, `shared_mutex` 완전 제거
- D5: send_error 헬퍼 11개 추출 (Auth 3 + Chat 8, 보일러플레이트 33곳 제거)

**Phase C — 최적화**
- D6: ConsumerPayloadPool (thread-safe 메모리 풀, Kafka consumer 전용)

**Phase D — Auto-Review**
- 5명 리뷰어 병렬 (design, systems, logic, test, infra-security)
- design: ServiceRegistry 미등록 수정, ConfigureContext 개선 (bind_io_context 분리)
- logic: refresh token expiry 체크 우회(보안 CRITICAL) + 4건 수정
- infra-security: GCC 호환 include 2건 추가
- test: D7 spawn 테스트 6건 추가
- systems: 이슈 0건 (메모리+동시성 안전성 확인)
