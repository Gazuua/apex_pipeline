# notify merge 통합 설계 — 머지 유일 진입점

- **백로그**: BACKLOG-234
- **브랜치**: feature/notify-merge-integration
- **날짜**: 2026-03-26

## 1. 문제

현재 머지 워크플로우에서 에이전트가 6단계를 수동으로 실행한다:

```
queue merge acquire → rebase → build → push → gh pr merge → queue merge release
```

`handoff notify merge`는 상태 정리(active → history)만 수행하고, `gh pr merge` 자체는 에이전트가 직접 호출한다. 이로 인해:

1. **우회 경로 존재**: 미등록 브랜치에서 `gh pr merge`를 직접 호출하면 `validate-handoff` hook이 통과시킴 (`isGitCommit` 체크에 걸리지 않음)
2. **notify merge 스킵 가능**: FIXING 백로그가 0건이면 `gh pr merge` → `notify merge` 없이 머지 완료 → 핸드오프 DB에 레코드 잔류
3. **워크플로우 원자성 부재**: lock 획득~해제가 에이전트 행동에 의존하여 실패 시 lock 누수 가능

## 2. 설계

### 2.1 핵심 변경

`handoff notify merge`를 머지의 **유일한 진입점**으로 통합한다. 데몬 파이프라인에서 lock 획득부터 checkout main까지 원자적으로 수행하고, `gh pr merge` 직접 호출은 hook에서 전면 차단한다.

### 2.2 데몬 파이프라인

**실행 주체**: 데몬이 전체 파이프라인을 실행한다. CLI는 `notify merge --summary "..."` IPC 요청만 보내고, 데몬이 IPC 요청에 포함된 `project_root`를 사용하여 git/gh 명령을 `exec.Command`로 실행한다. 이미 기존 파이프라인(`MergePipeline`)이 동일 방식으로 rebase, export commit, checkout main을 수행하고 있으므로 패턴 변경 없음.

`notify merge --summary "..."` 호출 시 데몬이 실행하는 파이프라인:

| # | 단계 | 명령 | 실패 시 |
|---|------|------|---------|
| ① | merge lock acquire | 내부 | 대기 (기존 동작) |
| ② | backlog export + commit | `backlog export` → `git add` → `git commit` | 에러 반환, lock release |
| ③ | rebase | `git fetch origin main` → `git rebase origin/main` | `git rebase --abort`, 에러 반환, lock release |
| ④ | push | `git push --force-with-lease` | 에러 반환, lock release |
| ⑤ | merge | `gh pr merge --squash --delete-branch --admin` | 에러 반환, lock release |
| ⑥ | finalize | active_branches → branch_history (MERGED) | 에러 반환 (exit 1) + 가이드 출력 |
| ⑦ | checkout main | `git checkout main` → `git pull origin main` | 경고 출력 (exit 0) + 가이드 출력 |
| ⑧ | lock release | 내부 (defer) | 항상 실행 |

**주**: ② backlog export 시 변경이 없으면 commit은 no-op (에러 아님). `git diff --cached --quiet`의 ExitError(변경 있음)와 실행 에러를 구분하여 처리.

**에러 처리:**

| Case | 단계 | exit code | 동작 | 에이전트 가이드 |
|------|------|:---------:|------|----------------|
| A | ①~⑤ 실패 | 1 | 롤백 (③ 실패 시 `rebase --abort`), lock release | 원인 해결 후 `notify merge` 재실행 |
| B | ⑥ finalize 실패 | 1 | 머지 완료됨, DB 상태만 불일치 | `handoff notify merge --summary "..."` 재실행 (멱등: 이미 머지된 브랜치는 정리만 수행) |
| C | ⑦ checkout 실패 | 0 | 머지+정리 모두 완료, 로컬 브랜치만 전환 안 됨 | `git checkout main && git pull origin main` 수동 실행 |

**멱등성**: ⑥ finalize는 active_branches에 레코드가 있을 때만 이관. 이미 이관됐으면 no-op. 따라서 Case B에서 재실행 시 안전.

### 2.3 gh pr merge 직접 호출 차단

**`validate-merge` hook 변경:**
- `gh pr merge` 감지 시 → 무조건 차단
  - 메시지: `"gh pr merge 직접 호출 금지 — apex-agent handoff notify merge --summary \"...\"를 사용하세요"`
- merge lock 확인 로직 제거 (데몬 내부에서 처리)
- `gh pr create` 검증은 그대로 유지

**`validate-handoff` hook (defense in depth):**
- `gh pr merge` 감지 시 `ValidateMergeGate` IPC 호출 **유지** — validate-merge가 entry gate, validate-handoff가 daemon-side gate로 다층 방어. FIXING 백로그 최종 검증은 데몬에서 수행

### 2.4 CLI 변경

**변경:**
- `handoff notify merge --summary "..."`: 내부에서 전체 파이프라인 실행

**제거:**
- `queue merge acquire` — 데몬 내부로 이동
- `queue merge release` — 데몬 내부로 이동
- `queue merge status` — 에이전트가 사용할 일 없음

**유지:**
- `queue build`, `queue benchmark` — 빌드는 에이전트가 별도 수행
- `queue acquire/release/status <channel>` — 범용 채널은 그대로

### 2.5 문서 갱신

| 문서 | 갱신 내용 |
|------|----------|
| `CLAUDE.md` (루트) | § 머지 절차를 `notify merge` 원커맨드로 교체, `queue merge acquire/release` 언급 제거 |
| `apex_tools/apex-agent/CLAUDE.md` | § 핸드오프 CLI (notify merge 파이프라인 명시), § 큐 CLI (merge 서브커맨드 제거), § Hook 게이트 (validate-merge 역할 변경) |
| `docs/Apex_Pipeline.md` | 로드맵/변경 이력 반영 |
| `docs/BACKLOG.json` | 파이프라인 ② 에서 자동 export (에이전트 수동 export 불필요) |

## 3. 에이전트 워크플로우 변경

**Before:**
```
⑥ 문서 갱신 → backlog export → commit → push
⑦ queue merge acquire
   → (enforce-rebase hook이 push 시 rebase)
   → queue build debug (선택)
   → git push --force-with-lease
   → gh pr merge --squash --delete-branch --admin
   → queue merge release
```

**After:**
```
⑥ 문서 갱신 → commit → push (export는 notify merge가 처리)
⑦ handoff notify merge --summary "..."  (끝)
```
