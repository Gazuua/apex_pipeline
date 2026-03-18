# BACKLOG 리팩토링 완료 기록

**브랜치**: `feature/backlog_refactoring`
**완료일**: 2026-03-18

## 작업 요약

BACKLOG.md를 2축 분류 체계(시간축 × 내용축)로 리팩토링하고, 전체 문서와 구현 간의 정합성을 검증·수정 완료.

## 마이그레이션 통계

| 구분 | 건수 |
|------|------|
| **총 검증 항목** | ~54건 (3개 소스) |
| **활성 항목 (BACKLOG.md)** | 47건 |
| — NOW | 1건 (CRITICAL 1) |
| — IN VIEW | 19건 (CRITICAL 2, MAJOR 17) |
| — DEFERRED | 27건 (MAJOR 3, MINOR 24) |
| **아카이브 (BACKLOG_HISTORY.md)** | 11건 |
| — FIXED | 7건 |
| — WONTFIX | 3건 |
| — SUPERSEDED | 1건 |
| **통합된 중복** | 5건 |

## 소스별 처리

- **docs/BACKLOG.md** (~30건): 코드베이스 대조 검증 후 새 포맷으로 재분류
- **docs/apex_core/backlog_memory_os_level.md** (5건): 3건 중복 통합, 1건 WONTFIX 아카이브, 1건 IN VIEW 승격 → 원본 삭제
- **docs/apex_shared/review/20260315_094300_backlog.md** (~19건): 전수 검증 후 IN VIEW 7건 + DEFERRED 10건 배치 → 원본 삭제

## 정합성 검증 결과

### 레이어 A — BACKLOG 항목 유효성
- 47건 활성 항목 전수 코드베이스 대조 검증 완료
- 11건 해결/해당없음 식별 → BACKLOG_HISTORY.md 아카이브

### 레이어 B — Apex_Pipeline.md 로드맵 정합성
- v0.5.5.1 기술 내용 90% 정확 확인
- v0.5.4.0 E2E 테스트 수 정정 (6→11 확장 반영)
- 로드맵과 BACKLOG NOW/IN VIEW 항목 간 모순 없음

### 레이어 C — plans/progress/review 추적성
- 62% 직접 매핑 (45/73 plans에 대응 progress 존재)
- 갭 대부분 deferred 작업(Phase 5.5 tier plans) 또는 초기 레거시
- 중복 파일 1건 삭제 (apex_infra/plans/ → apex_shared/plans/에 원본 보존)

### 레이어 D — CLAUDE.md 규칙 정합성
- apex_core/CLAUDE.md build.bat 경로 오류 수정 (CRITICAL)
- 2축 백로그 체계가 루트 + docs/CLAUDE.md에 정상 반영 확인
- 전체 CLAUDE.md 파일 간 모순 없음

## 신설 파일

- `docs/BACKLOG_HISTORY.md` — 완료 항목 아카이브 (prepend 방식, 마커 기반)
- `docs/apex_common/plans/20260318_122201_backlog-refactoring-design.md` — 설계서
- `docs/apex_common/plans/20260318_123309_backlog-refactoring-plan.md` — 구현 계획

## 수정 파일

- `docs/BACKLOG.md` — 2축 체계 전면 교체
- `docs/CLAUDE.md` — 백로그 운영 가이드라인 섹션 추가
- `CLAUDE.md` (루트) — 백로그 규칙 2축 체계로 교체
- `apex_core/CLAUDE.md` — build.bat 경로 수정
- `docs/Apex_Pipeline.md` — E2E 테스트 수 정정

## 삭제 파일

- `docs/apex_core/backlog_memory_os_level.md` — BACKLOG.md 통합 완료
- `docs/apex_shared/review/20260315_094300_backlog.md` — BACKLOG.md 통합 완료
- `docs/apex_infra/plans/20260307_202237_monorepo-infra-and-shared.md` — 중복 삭제
