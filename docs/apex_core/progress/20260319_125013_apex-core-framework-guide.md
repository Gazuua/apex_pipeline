# apex_core 프레임워크 가이드 완료 기록

**백로그**: #1
**브랜치**: `feature/1-apex-core-framework-guide`
**버전**: v0.5.5.2 (문서 전용, 프레임워크 버전 변경 없음)

---

## 산출물

| 파일 | 내용 |
|------|------|
| `docs/apex_core/apex_core_guide.md` | 프레임워크 가이드 본체 (§1-§11, ~1,080줄) |
| `docs/apex_core/plans/20260319_114516_apex-core-framework-guide-spec.md` | 설계 스펙 |
| `docs/apex_core/plans/20260319_123254_apex-core-guide-impl.md` | 구현 계획 |
| `CLAUDE.md` | 유지보수 규칙 + 가이드 포인터 추가 |

## 가이드 구조

2레이어, 태스크 기반 단일 파일. 에이전트가 위에서 아래로 따라가면 서비스 완성.

**레이어 1 (§1-§9)**: 퀵 레퍼런스 스켈레톤, ServerConfig, 라이프사이클 Phase 1-3.5, 핸들러 4종 시그니처+예시, 어댑터 접근, 메모리 관리 (bump/arena), 유틸리티 (cross_core, scheduler, spawn), 금지사항 7종 BAD/GOOD, CMake 빌드 템플릿.

**레이어 2 (§10)**: 컴포넌트 배치도, Phase 시퀀스, TCP/Kafka 요청 흐름, ADR 포인터 10개.

**부록 (§11)**: Gateway/Kafka-only/다중역할/응답전송 4가지 실전 패턴.

## 아키텍처 결정 (D1-D7)

#48 코드 리뷰 에이전트 핸드오프 기반으로 7건의 설계 결정 수행. 가이드에 "의도된 설계"로 기술, 코드 구현은 #48 담당.

| ID | 결정 |
|----|------|
| D1 | ServiceRegistry 코어 자동 등록, `registry.get<T>()` 정석 |
| D2 | Kafka 자동 배선, post_init_callback Kafka용 폐기 |
| D3 | `server.global<T>(factory)` + raw ptr, shared_ptr 금지 |
| D4 | shared-nothing 예외 없음, shared_mutex 금지 |
| D5 | send_response 패턴 가이드, 헬퍼는 서비스 자율 |
| D6 | Kafka consumer 전용 메모리 풀, 코어 관리 |
| D7 | `spawn()` tracked API, io_context 미노출 |

## 백로그 변동

- **삭제**: #1 (완료 → BACKLOG_HISTORY)
- **신규**: #63 (CLAUDE.md 중복 정리), #64 (테스트 가이드), #65 (auto-review 가이드 검증)
