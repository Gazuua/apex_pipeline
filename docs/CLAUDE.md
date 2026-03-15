# docs — 문서 작성 & 리뷰 가이드

## 문서 규칙

- **필수 작성**: 계획서(`plans/`), 완료 기록(`progress/`), 리뷰 보고서(`review/`)
- **작성 타이밍**: plans → 구현 전, review → 리뷰 완료 후, progress → CI 통과 후 merge 전
- **문서 위치**: 프로젝트 전용 → `docs/<project>/`, 공통 → `docs/apex_common/`, 걸치는 문서 → 양쪽에 관점 조정하여 작성 (단순 복사 금지)

### 설계/계획 문서 경로
- 설계 스펙, 구현 계획 등 프로젝트 문서는 `docs/{project}/plans/`에 저장
- superpowers 스킬의 기본 경로(`docs/superpowers/`)를 사용하지 않음 — 프로젝트 문서 규칙 우선
- 예: `docs/apex_core/plans/20260315_000000_v0.5-wave1-design.md`

- 파일명: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 파일 생성 직전 `date +%Y%m%d_%H%M%S` 명령으로 취득한 **정확한 현재 시각**. 추정/반올림/대략적 시간 사용 금지
- 리뷰·progress 문서에서 발견된 TODO/백로그 → `docs/BACKLOG.md`에 추가
- 원본 문서(review/progress)에 TODO·백로그·향후 과제 섹션 잔류 금지 — 발견 즉시 `docs/BACKLOG.md`로 이전 후 원본에서 제거
- BACKLOG 항목 완료 시 해당 항목 즉시 삭제 (git이 이력 보존)

## 코드 리뷰

- **clangd LSP + superpowers:code-reviewer 병행** — LSP 정적 분석(타입/참조/호출 추적)과 AI 코드 리뷰를 함께 사용해야 품질이 높아진다
- **clangd LSP 효율 전략**: `documentSymbol` 병렬 → 핵심 API `hover` → 의심 패턴 `findReferences`/`incomingCalls`. 전수 분석 금지, 10분 타임아웃.
- **설계 문서 정합성**: 아키텍처 영향 변경 시 `Apex_Pipeline.md` 일치 확인 필수

## 브레인스토밍

- 1단계에서 반드시 `docs/Apex_Pipeline.md` 읽고 관련 섹션 식별, 설계 후 업데이트 포함
- `docs/BACKLOG.md`도 확인, 관련 항목 있으면 이번 작업에 포함 검토
