# BACKLOG-108: 브랜치 핸드오프 시스템 테스트 + 강제화

## 요약

`branch-handoff.sh`의 전체 워크플로우를 실전 테스트하고, 발견된 5건의 버그/개선점을 수정했다.
에이전트 채택률을 높이기 위해 hook 기반 자동화 3종을 구현하고, CLAUDE.md 핸드오프 지침을 14줄→5줄로 정리했다.

## 테스트 결과 (전 명령어 검증 완료)

| 명령어 | 테스트 항목 | 결과 |
|--------|-----------|------|
| `notify start` | Tier 1 알림, active/backlog-status 생성 | PASS |
| `notify design` | Tier 2 알림, payload 생성, SUPERSEDE | PASS |
| `notify merge` | Tier 3 알림, active/backlog-status 삭제 | PASS |
| `check` | 자기 알림 제외, 스코프 필터링, SUPERSEDE 제외 | PASS |
| `check --gate` | BLOCKED/CLEAR 판정, 스코프+게이트 조합 | PASS |
| `ack` | 유효/무효 action, 응답 파일 생성, ack 후 필터링 | PASS |
| `read` | payload 조회, 미존재 ID 에러 | PASS |
| `backlog-check` | 자기/타 브랜치 탐지, 미등록 ID, 인자 없음 | PASS |
| `status` | active 브랜치, backlog 목록, 알림 수 | PASS |
| `cleanup` | index compaction, archive 이동 | PASS |
| 에러 핸들링 | 잘못된 커맨드, 인자 누락 | PASS |

## 발견 및 수정한 버그/개선점

### 1. `--description` 파라미터 미지원 (BUG → FIXED)
- `parse_args()`가 `--summary`만 인식, `--description`은 `*) shift ;;`로 조용히 무시
- 수정: `--summary|--description` alias 추가

### 2. 미인식 파라미터 무시 (BUG → FIXED)
- `*) shift ;;`가 모든 미인식 옵션을 경고 없이 소실
- 수정: `--*` 패턴 추가 → `[handoff] WARNING: unknown option` stderr 출력

### 3. 빈 스코프 알림 스코프 필터 매칭 실패 (IMPROVEMENT → FIXED)
- scopes 미지정 알림이 `check --scopes tools` 시 매칭 안 됨
- scopes 비어있으면 "전체 스코프"로 간주 → 항상 매칭

### 4. BACKLOG-N 중복 접두어 (IMPROVEMENT → FIXED)
- summary에 이미 `BACKLOG-N`이 포함된 경우 자동 접두어가 중복 추가
- 수정: 기존 포함 여부 체크 후 조건부 추가

### 5. branch_01 scopes/summary 비어있음 (OBSERVATION)
- #107 에이전트가 `--scopes`/`--summary` 없이 `notify start` 호출
- → hook 기반 자동화의 필요성 확인

## 신규 구현: Hook 기반 강제화 (3종)

### A. SessionStart hook 확장 (`session-context.sh`)
- main 브랜치: 자동 `git fetch origin main && git pull origin main`
- 비-main 브랜치: 핸드오프 미등록 시 WARNING 출력 + 미처리 알림 자동 표시

### B. PreToolUse: 머지 gate check (`validate-handoff.sh`)
- `gh pr merge` 시 `check --gate merge` 자동 실행
- 미ack 알림 있으면 exit 2로 차단 + 해결 방법 안내

### C. PreToolUse: 브랜치 생성 리마인더 (`validate-handoff.sh`)
- `git checkout -b` / `git switch -c` 감지
- `notify start` 실행 리마인더 출력 (차단 없음)

## 변경 파일

| 파일 | 변경 내용 |
|------|----------|
| `apex_tools/branch-handoff.sh` | --description alias, 미인식 옵션 경고, 빈 스코프 매칭, BACKLOG-N 중복 방지 |
| `apex_tools/session-context.sh` | main 자동 최신화 + 핸드오프 상태 섹션 추가 |
| `.claude/hooks/validate-handoff.sh` | 신규 — 머지 gate check + 브랜치 생성 리마인더 |
| `.claude/settings.json` | validate-handoff.sh hook 등록 |
| `CLAUDE.md` | 핸드오프 지침 정리 (14줄→5줄, hook 강제 항목 제거) |
