# Auto-Review 설정

## 전역 설정

| 항목 | 값 | 설명 |
|------|-----|------|
| round_limit | 10 | 전체 라운드 상한 |
| same_issue_limit | 5 | 동일 이슈 반복 상한 (초과 시 에스컬레이션) |
| ci_retry_limit | 5 | 동일 CI 실패 재시도 상한 |

## 스마트 스킵 매핑 테이블

### 1단계: 데이터 매칭 (파일 패턴 -> 리뷰어 자동 선정)

| 파일 패턴 | 영향 리뷰어 |
|-----------|------------|
| `.cpp`, `.hpp` (src/) | logic, memory, concurrency, api, test-coverage |
| `test_*.cpp` | test-coverage, test-quality, logic |
| `*allocator*`, `*pool*` | memory |
| `*coroutine*`, `*async*` | concurrency |
| `concept*`, `PoolLike*` | api |
| `CMakeLists.txt`, `vcpkg.*` | infra |
| `*.yml` (CI), `Dockerfile` | infra |
| `*.md` (docs/) | docs-spec, docs-records, architecture |
| `Apex_Pipeline.md` | docs-spec, architecture |
| `README.md`, `CLAUDE.md` | docs-spec |
| `plans/`, `progress/`, `review/` | docs-records |
| `*.sh`, `*.bat` (scripts) | infra |
| `.gitignore`, `.gitattributes` | infra |
| `*suppressions.txt` | infra, test-quality |
| `.toml`, `.sql` | infra |
| `.fbs` (FlatBuffers) | logic, architecture |

### 폴백 규칙

- 위 매핑에 해당하지 않는 파일 -> `infra` 자동 포함
- 판단이 애매하면 -> 해당 리뷰어를 포함 (보수적)

### 2단계: 팀장 재량 판단

데이터 매핑은 팀장 판단의 **출발점**. 팀장이 diff 내용을 분석하여 **추가도 스킵도** 할 수 있다.

- **추가**: 매핑에 안 걸렸지만 리뷰가 필요한 리뷰어 추가 디스패치
  - 예: `.hpp` 변경이지만 보안 관련 입력 검증 코드 -> security 추가
- **스킵**: 매핑에 걸렸지만 변경 내용상 리뷰가 불필요한 리뷰어 스킵
  - 예: 테스트 코드 오타 수정 -> test-quality만 남기고 나머지 스킵
- **full 모드**: 전원 디스패치 (팀장 재량 스킵 없음)

## 재리뷰 스마트 스킵

수정이 발생한 라운드 이후, 재리뷰 대상을 결정하는 규칙.

### 리뷰어 보고 필드: `re_review_scope`

리뷰어가 수정 보고 시 반드시 포함하는 영향도 판단:

| 값 | 의미 | coordinator 행동 |
|----|------|-----------------|
| `self_contained` | 수정이 로컬에 한정, 외부 영향 없음 | 파일 매핑 보수적 검증만 |
| `same_domain` | 같은 도메인 내 재리뷰 필요 | 같은 리뷰어가 재검증 |
| `cross_domain` | 다른 도메인에도 영향 | `affected_domains` 리뷰어 재리뷰 |

### 재리뷰 대상 결정 로직

1. 모든 리뷰어의 `re_review_scope` 취합
2. 수정된 파일을 §1단계 매핑 테이블에 대입 → 영향 리뷰어 후보
3. `re_review_scope` 기반 필터링:
   - 전부 `self_contained` → 매핑 후보 중 직전 라운드 Clean 리뷰어 스킵 가능
   - `same_domain` → 해당 리뷰어 재리뷰
   - `cross_domain` → `affected_domains` + 매핑 후보 합집합
4. 수정 0건 → Clean 판정
5. 수정 발생 → 위 과정 반복 (round_limit까지)

## 리뷰어 목록

| 리뷰어 | 도메인 | 파일 |
|--------|--------|------|
| docs-spec | 원천 문서 정합성 | `apex_tools/claude-plugin/agents/reviewer-docs-spec.md` |
| docs-records | 기록 문서 품질 | `apex_tools/claude-plugin/agents/reviewer-docs-records.md` |
| architecture | 설계 <-> 코드 정합 | `apex_tools/claude-plugin/agents/reviewer-architecture.md` |
| logic | 비즈니스 로직 | `apex_tools/claude-plugin/agents/reviewer-logic.md` |
| memory | 메모리 관리 | `apex_tools/claude-plugin/agents/reviewer-memory.md` |
| concurrency | 동시성 | `apex_tools/claude-plugin/agents/reviewer-concurrency.md` |
| api | API/concept 설계 | `apex_tools/claude-plugin/agents/reviewer-api.md` |
| test-coverage | 테스트 커버리지 | `apex_tools/claude-plugin/agents/reviewer-test-coverage.md` |
| test-quality | 테스트 코드 품질 | `apex_tools/claude-plugin/agents/reviewer-test-quality.md` |
| infra | 빌드/CI/인프라 | `apex_tools/claude-plugin/agents/reviewer-infra.md` |
| security | 보안 | `apex_tools/claude-plugin/agents/reviewer-security.md` |
