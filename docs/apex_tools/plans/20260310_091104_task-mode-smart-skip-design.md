# Task 모드 Round 1 스마트 스킵 설계

## 개요

auto-review task 모드의 Phase 1 Round 1에서 변경 파일 타입 기반으로 관련 없는 리뷰어 에이전트를 스킵하는 기능.
현재 Round 1은 모드와 무관하게 5개 전원 디스패치하지만, task 모드에서는 변경 범위가 명확하므로 불필요한 에이전트를 건너뛸 수 있다.

## 동기

- task 모드는 `git diff main...HEAD`로 변경 파일이 명확히 결정됨
- 문서만 바꿨는데 code/test/structure/general 4개를 더 돌리는 건 낭비
- Phase 2 재리뷰 루프에는 이미 스마트 스킵이 있지만, Round 1에는 없음

## 설계

### 변경 범위

`auto-review.md` 한 파일. Phase 1 섹션에 task 모드 분기 추가 + 파일타입 매핑 테이블을 공용 섹션으로 승격.

### 파일타입 → 리뷰어 매핑 (공용)

Phase 1 task 모드 Round 1과 Phase 2 재리뷰 루프가 동일한 매핑을 참조한다.

| 수정 파일 타입 | 영향받는 리뷰어 |
|---|---|
| `.cpp`, `.hpp` (소스/헤더) | code, test, structure |
| `test_*.cpp`, `test_helpers.hpp` | test |
| `.fbs` (FlatBuffers 스키마) | code, structure |
| `CMakeLists.txt`, `vcpkg.json`, `build.*`, `CMakePresets*` | structure, general |
| `Dockerfile`, `.dockerignore`, `docker-compose.yml` | structure, general |
| `.github/workflows/*.yml` (CI) | general |
| `*suppressions.txt` (TSAN/LSAN) | general, test |
| `.toml`, `.sql` (설정/DB) | general |
| `.clangd`, `.gitattributes`, `.editorconfig` | general |
| `*.md` (문서) | docs |
| `.gitignore`, hooks, scripts (`.sh`, `.bat`) | general |

### 폴백 규칙

- 매핑에 안 걸리는 파일이 하나라도 있으면 → `general` 자동 포함
- `full` 모드 Round 1은 변경 없음 (전원 디스패치 유지)

### Phase 1 동작 흐름 (task 모드)

1. `git diff --name-only main...HEAD`로 변경 파일 추출 (기존)
2. 변경 파일 목록에 매핑 테이블 적용 → 필요한 리뷰어 집합 결정
3. 해당 리뷰어만 디스패치

### 리포트 포맷

```markdown
## Round 1 참여 현황 (task 모드 스마트 스킵)
- 변경 파일: docs/apex_core/plans/xxx.md, README.md
- reviewer-docs: 리뷰 수행 (*.md 매칭)
- reviewer-structure: 스킵 (변경 파일 무관)
- reviewer-code: 스킵 (변경 파일 무관)
- reviewer-test: 스킵 (변경 파일 무관)
- reviewer-general: 기본 포함
```

### full 모드와의 차이

| 항목 | task 모드 | full 모드 |
|---|---|---|
| Round 1 | 매핑 기반 선택 디스패치 | 5개 전원 디스패치 |
| Round 2+ | Phase 2 스마트 스킵 (기존) | Phase 2 스마트 스킵 (기존) |
