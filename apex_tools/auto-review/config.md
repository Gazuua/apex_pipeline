# Auto-Review 설정

메인 에이전트의 리뷰어 디스패치 판단을 위한 참고 가이드.
강제 절차가 아닌 참고값 — 메인이 diff 분석 후 최종 결정.

## 전역 참고값

| 항목 | 참고값 | 설명 |
|------|--------|------|
| round_limit | 10 | 전체 라운드 상한 |
| same_issue_limit | 5 | 동일 이슈 반복 상한 (초과 시 백로그 이관 검토) |
| ci_retry_limit | 5 | 동일 CI 실패 재시도 상한 |

## 공통 지침: 프로젝트 규칙 위반 = Critical

> **모든 리뷰어 공통 필수 규칙**: 프로젝트 규칙(`CLAUDE.md`, `docs/CLAUDE.md`, `apex_core/CLAUDE.md` 등에 명시된 네이밍 규칙, 경로 규칙, 커밋 규칙 등)에 위배되는 이슈는 severity와 무관하게 **무조건 Critical**로 판단하고 **즉시 수정**하라. 백로그로 넘기지 않는다.

## 스마트 스킵 매핑 테이블

파일 패턴 → 리뷰어 자동 선정 참고. 메인이 diff 내용을 분석하여 추가/스킵 가능.

| 파일 패턴 | 영향 리뷰어 |
|-----------|------------|
| `.cpp`, `.hpp` (src/) | logic, systems, design, test |
| `test_*.cpp` | test, logic |
| `*allocator*`, `*pool*` | systems |
| `*coroutine*`, `*async*` | systems |
| `concept*`, `PoolLike*` | design |
| `CMakeLists.txt`, `vcpkg.*` | infra-security |
| `*.yml` (CI), `Dockerfile` | infra-security |
| `*.md` (docs/) | docs-spec, docs-records, design |
| `Apex_Pipeline.md` | docs-spec, design |
| `README.md`, `CLAUDE.md` | docs-spec |
| `plans/`, `progress/`, `review/` | docs-records |
| `*.sh`, `*.bat` (scripts) | infra-security |
| `.gitignore`, `.gitattributes` | infra-security |
| `*suppressions.txt` | infra-security, test |
| `.toml`, `.sql` | infra-security |
| `.fbs` (FlatBuffers) | logic, design |

### 폴백 규칙

- 위 매핑에 해당하지 않는 파일 → `infra-security` 자동 포함
- 판단이 애매하면 → 해당 리뷰어를 포함 (보수적)

## 리뷰어 목록 (7명)

| 리뷰어 | 도메인 | 프롬프트 |
|--------|--------|----------|
| docs-spec | 원천 문서 정합성 | `apex_tools/auto-review/agents/reviewer-docs-spec.md` |
| docs-records | 기록 문서 품질 | `apex_tools/auto-review/agents/reviewer-docs-records.md` |
| design | 설계/API/아키텍처 정합 | `apex_tools/auto-review/agents/reviewer-design.md` |
| logic | 비즈니스 로직 | `apex_tools/auto-review/agents/reviewer-logic.md` |
| systems | 메모리/동시성/저수준 | `apex_tools/auto-review/agents/reviewer-systems.md` |
| test | 테스트 커버리지+품질 | `apex_tools/auto-review/agents/reviewer-test.md` |
| infra-security | 빌드/CI/인프라/보안 | `apex_tools/auto-review/agents/reviewer-infra-security.md` |
