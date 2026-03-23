# cleanup 자기 워크트리 삭제 버그 핫픽스 + 핸드오프 구조 강화

**브랜치**: `bugfix/cleanup-self-destruct`
**백로그**: 신규 (기존 #146과 무관)
**작성일**: 2026-03-23

---

## 1. 배경

### 사고 경위

cleanup --execute 실행 시 현재 워크트리(branch_01)를 "머지 완료 브랜치"로 판단하고 디렉토리째 삭제.
원인: 빈 커밋 없이 main에서 생성된 브랜치를 `IsMergedToMain`이 "머지 완료"로 판정 + 현재 워크트리/핸드오프 상태 확인 로직 부재.

### 발견된 이슈

| # | 이슈 | 심각도 |
|---|------|--------|
| 1 | cleanup이 현재 워크트리를 삭제 | CRITICAL |
| 2 | 핸드오프 UNIQUE 충돌 — branches 레코드 미정리 | MAJOR |
| 3 | cleanup이 핸드오프 상태를 무시 | MAJOR |

이슈 3은 이슈 1의 해결책(cleanup–핸드오프 연동)에 포함되어 자동 해결.

---

## 2. 설계 결정

### 2.1 핸드오프 기반 브랜치 생성 (이슈 1 근본 해결)

**원칙**: 모든 브랜치는 핸드오프 등록과 원자적으로 생성된다. 핸드오프 없는 브랜치는 존재하지 않는다.

**흐름**:
```
notify start --branch-name feature/foo --backlog 146 --scopes core --summary "..."
│
├─ Phase 1: DB TX (원자적)
│   INSERT active_branches (status=STARTED, git_branch="feature/foo")
│   INSERT branch_backlogs
│   UPDATE backlog_items (OPEN → FIXING)
│   INSERT notifications
│   ── TX COMMIT ──
│
├─ Phase 2: Git (재시도 가능)
│   git checkout -b feature/foo
│   git push -u origin feature/foo
│
└─ 실패 복구:
    Phase 1 실패 → 재시도
    Phase 2 실패 → DB 레코드가 브랜치를 보호, git만 재시도
                    (같은 workspace에서 재실행 시 기존 재등록 로직이
                     git_branch 기준으로 기존 레코드를 탐지·정리 후 재등록)
```

**DB 먼저 → git 후 순서의 안전성**:
- DB 레코드가 "브랜치 예약"으로 작동 — cleanup이 active_branches 조회 시 보호됨
- git 실패 시 DB 레코드가 잔존하지만, 다음 시도에서 git만 재실행하면 됨
- 보상 트랜잭션 불필요 (saga pattern 단순화)

**validate-build hook 확장** — 직접 브랜치 생성 차단:
- 차단: `git checkout -b`, `git checkout -B`, `git switch -c`, `git switch --create`, `git branch <name>`, `git worktree add -b`
- 허용: `git branch`(인자 없음, 조회), `git branch -D`/`-d`(삭제), `git branch -a`/`-v`/`--list`(조회)
- `git branch <name>` 판별: 플래그(`-`로 시작)가 아닌 인자가 있으면 브랜치 생성으로 간주
- 에러 메시지: `"차단: 직접 브랜치 생성 금지. 'apex-agent handoff notify start --branch-name <name>' 을 사용하세요."`

**notify start API 강화**:

| 파라미터 | 필수 | 검증 |
|----------|------|------|
| `--branch-name` | 필수 | `^(feature\|bugfix)/[a-z0-9][a-z0-9_-]*$` 패턴 (언더스코어 허용) |
| `--summary` | 필수 | 비어있으면 거부 |
| `--scopes` | 필수 (격상) | 알려진 스코프 태그 검증 |
| `--backlog` | start에서 필수, job에서 불가 | 존재 + OPEN 상태 확인 |
| `--skip-design` | 선택 | - |

서버 측 추가 검증:
- branch-name이 active_branches에 이미 존재 (PK 또는 git_branch UNIQUE) → 거부
- branch-name이 로컬/리모트 git에 이미 존재 → 거부
- backlog가 FIXING 상태 → 거부
- scopes에 유효하지 않은 태그 → 거부

### 2.2 DB 스키마 변경 (이슈 2 해결)

**테이블 리네이밍**:
- `branches` → `active_branches` (활성 작업 레코드)
- 신규 `branch_history` (완료/포기 이력)

```sql
CREATE TABLE active_branches (
    branch      TEXT PRIMARY KEY,
    workspace   TEXT NOT NULL,
    git_branch  TEXT UNIQUE,     -- 동일 git 브랜치의 중복 등록 방지
    status      TEXT NOT NULL,   -- STARTED, DESIGN_NOTIFIED, IMPLEMENTING
    summary     TEXT,
    created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    updated_at  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);

CREATE TABLE branch_history (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    branch      TEXT NOT NULL,
    workspace   TEXT NOT NULL,
    git_branch  TEXT,
    status      TEXT NOT NULL,   -- MERGED, DROPPED
    summary     TEXT,
    backlog_ids TEXT,             -- JSON 배열 스냅샷
    started_at  TEXT NOT NULL,
    finished_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
```

**MERGE_NOTIFIED 상태 폐기**:
- `active_branches`의 유효 상태: `STARTED`, `DESIGN_NOTIFIED`, `IMPLEMENTING` (3개만)
- 머지/포기 시 레코드가 `branch_history`로 이관되므로 `MERGE_NOTIFIED` 상태 불필요
- `state.go`에서 `StatusMergeNotified` 상수 제거, `NextStatus`의 `IMPLEMENTING + merge` 경로 제거
- `CanEditSource`에서 `MERGE_NOTIFIED` 조건 제거
- `BacklogCheck`에서 `status != MERGE_NOTIFIED` 제외 조건 제거 (active_branches에 있는 건 전부 활성)

**테이블 RENAME 영향 범위** — `branches` → `active_branches` 일괄 변경 대상:
- `modules/handoff/manager.go`: 모든 SQL 쿼리 (~15개 쿼리)
- `modules/handoff/gate.go`: ValidateMergeGate, ValidateCommit 등의 쿼리
- `modules/handoff/module.go`: RegisterSchema DDL + 마이그레이션
- `cli/handoff_cmd.go`: 직접 쿼리가 있다면 변경
- `e2e/handoff_test.go`: 테스트 내 직접 DB 참조

**마이그레이션 전략 (v5)**:
1. `ALTER TABLE branches RENAME TO active_branches`
2. `CREATE TABLE branch_history`
3. `active_branches`에 `git_branch` UNIQUE 제약 추가 (기존 컬럼이므로 테이블 재생성 필요할 수 있음)
4. MERGE_NOTIFIED 레코드 → branch_history (status=MERGED) 이관 후 active_branches에서 삭제
5. 나머지 전부 → branch_history (status=DROPPED) 이관 후 active_branches에서 삭제
6. 현재 작업 브랜치 재등록: 마이그레이션 스크립트 내에서 자동 처리하지 않음.
   마이그레이션 후 에이전트가 `notify start`를 재실행하여 등록.
   (마이그레이션 시점에는 현재 workspace/브랜치 정보를 알 수 없으므로)

### 2.3 notify drop 신규 커맨드

```bash
apex-agent handoff notify drop --reason "작업 방향 변경"
```

**흐름**:
```
notify drop
│
├─ Pre-check: FIXING 백로그 잔존 확인 (checkFixingBacklogs 공용 함수)
│   ├─ FIXING 존재 → 에러:
│   │   "FIXING 상태 백로그가 남아있습니다: #146, #147"
│   │   "먼저 backlog release 또는 backlog resolve 로 처리하세요"
│   │   → 실패 (exit 1)
│   └─ FIXING 없음 → 진행
│
├─ Phase 1: DB TX (원자적)
│   INSERT branch_history (status=DROPPED, backlog_ids 스냅샷)
│   DELETE branch_backlogs WHERE branch = ?
│   DELETE notification_acks WHERE branch = ? OR notification_id IN (...)
│   DELETE notifications WHERE branch = ?
│   DELETE active_branches WHERE branch = ?
│   ── TX COMMIT ──
│
└─ Phase 2: git checkout main && git pull origin main
```

### 2.4 notify merge 변경

**기존 NotifyTransition 경로에서 분리** — 별도 `NotifyMerge` 함수로 구현:
- 기존: `NotifyTransition(branch, workspace, "merge", summary)` → `NextStatus` → UPDATE status
- 변경: `NotifyMerge(branch, workspace, summary)` → DELETE + INSERT branch_history

**흐름**:
```
notify merge
│
├─ Pre-check: FIXING 백로그 잔존 확인 (checkFixingBacklogs 공용 함수)
│   ├─ FIXING 존재 → 에러 (notify drop과 동일 메시지)
│   └─ FIXING 없음 → 진행
│
├─ Phase 1: DB TX (원자적)
│   INSERT branch_history (status=MERGED, backlog_ids 스냅샷)
│   DELETE branch_backlogs WHERE branch = ?
│   DELETE notification_acks WHERE branch = ? OR notification_id IN (...)
│   DELETE notifications WHERE branch = ?
│   DELETE active_branches WHERE branch = ?
│   ── TX COMMIT ──
│
└─ Phase 2: git checkout main && git pull origin main
```

**FIXING 게이트 통합**: `checkFixingBacklogs(branch string) ([]int, error)` 공용 함수를 `manager.go`에 추출.
notify merge, notify drop 양쪽 모두 커맨드 내부에서 호출. 기존 `ValidateMergeGate`의 FIXING 체크도 이 함수를 사용하도록 통합.

**module.go 라우트 변경**: `"notify-transition"` 핸들러에서 merge 타입을 제거하고, `"notify-merge"` 별도 핸들러 등록.

### 2.5 cleanup 연동 (이슈 3 해결)

**cleanup.Run() 시그니처 변경**:
```go
func Run(repoRoot string, execute bool, activeBranches map[string]bool) (*Result, error)
```

각 Phase에 핸드오프 체크 추가:

| Phase | 기존 | 추가 |
|-------|------|------|
| Phase 1 (worktrees) | IsMergedToMain + isDirty | + activeBranches[wtBranch] → 스킵 |
| Phase 2 (local branches) | IsMergedToMain + dirtyBranches | + activeBranches[branch] → 스킵 |
| Phase 3 (remote branches) | IsMergedToMain | + activeBranches[branch] → 스킵 |

**activeBranches map 구성**: CLI에서 데몬 IPC `handoff.list-active` 호출 → `git_branch` 목록으로 map 구성.
`activeBranches`의 key는 **git_branch**(git 브랜치명) — cleanup은 git 브랜치명으로 비교하므로.

**handoff 모듈에 list-active 라우트 추가**:
```go
// 반환 JSON: [{"branch": "branch_agent", "git_branch": "feature/foo"}, ...]
"list-active" → SELECT branch, git_branch FROM active_branches
```

**CWD 보호 (defense-in-depth)**:
```go
// processWorktrees 내
cwd, _ := os.Getwd()
// isSubPath: filepath.Abs → filepath.Rel 기반 상대 경로 계산.
// Rel 결과가 ".."으로 시작하지 않으면 cwd가 wtPath 하위.
// Windows에서 대소문자 무시 + 드라이브 레터 정규화 포함.
if isSubPath(cwd, wtPath) {
    result.Warnings = append(result.Warnings, fmt.Sprintf("현재 작업 디렉토리: %s [%s] — 스킵", wtPath, wtBranch))
    continue
}
```

### 2.6 TxStore 타입 분리

현재 `Store`의 `tx *sql.Tx` 필드 패턴을 타입 레벨로 분리:

```go
// Store — DB 전용, 트랜잭션 시작만 담당
type Store struct {
    db *sql.DB
}
func (s *Store) RunInTx(ctx context.Context, fn func(tx *TxStore) error) error
func (s *Store) Exec/Query/QueryRow(...)  // 비트랜잭션 쿼리

// TxStore — 트랜잭션 전용, RunInTx 콜백 안에서만 존재
type TxStore struct {
    tx *sql.Tx
}
func (ts *TxStore) Exec/Query/QueryRow(...)  // 트랜잭션 쿼리
```

**영향 범위**:
- `RunInTx` 콜백 시그니처: `func(txs *store.Store)` → `func(tx *store.TxStore)` (전 모듈)
- `BacklogOperator.SetStatusWith`: `(txs *store.Store, ...)` → `(tx *store.TxStore, ...)`
- `MigrateFunc` 시그니처: `func(s *Store) error` → `func(tx *TxStore) error` (migrator.go)
- 전 모듈의 `RegisterSchema` 마이그레이션 콜백: 시그니처 일괄 변경
  - `modules/handoff/module.go` (~10개 마이그레이션 함수)
  - `modules/backlog/module.go` (~3개 마이그레이션 함수)
  - `modules/queue/module.go` (~2개 마이그레이션 함수)

### 2.7 txs.Exec 에러 무시 버그 수정

`NotifyStart`의 재등록 로직(기존 레코드 정리)에서 `txs.Exec` 반환값을 무시하는 버그 수정.
모든 `Exec` 호출에 에러 체크 추가, 실패 시 트랜잭션 롤백.

---

## 3. 핸드오프 개입 전체 흐름

```
워크플로우 단계          핸드오프 개입               트랜잭션 묶음
═══════════════════════════════════════════════════════════════════

① 착수 ─────────── notify start ──────── TX {
                    (브랜치 생성 포함)       INSERT active_branches
                                            INSERT branch_backlogs
                                            UPDATE backlog_items (OPEN→FIXING)
                                            INSERT notifications
                                          }
                                          POST-TX { git checkout -b, git push -u }

② 설계 ─────────── notify design ─────── TX {
                                            UPDATE active_branches (→DESIGN_NOTIFIED)
                                            INSERT notifications
                                          }

③ 구현 ─────────── notify plan ────────── TX {
                                            UPDATE active_branches (→IMPLEMENTING)
                                            INSERT notifications
                                          }

④ 검증 ─────────── (변경 없음) ──────────  -
⑤ 리뷰 ─────────── (변경 없음) ──────────  -
⑥ 문서 갱신 ────── (변경 없음) ──────────  -

⑦ 머지 ─────────── notify merge ──────── Pre-check: FIXING 잔존 → 차단
                                          TX {
                                            INSERT branch_history (MERGED)
                                            DELETE branch_backlogs
                                            DELETE notification_acks (관련)
                                            DELETE notifications (관련)
                                            DELETE active_branches
                                          }
                                          POST-TX { git checkout main, git pull }

✗ 포기 ──────────── notify drop ────────── Pre-check: FIXING 잔존 → 차단
                                          TX {
                                            INSERT branch_history (DROPPED)
                                            DELETE branch_backlogs
                                            DELETE notification_acks (관련)
                                            DELETE notifications (관련)
                                            DELETE active_branches
                                          }
                                          POST-TX { git checkout main, git pull }

═══════════════════════════════════════════════════════════════════
상시 Hook (PreToolUse)
═══════════════════════════════════════════════════════════════════
Bash:  validate-build ── cmake/ninja/build.bat 차단
                         + git checkout -b/-B, git switch -c, git branch <name>,
                           git worktree add -b 차단
       validate-merge ── merge lock 미획득 시 차단
       validate-handoff ── 미등록 커밋 차단, 머지 시 미ack/FIXING 차단

Edit/Write:
       validate-handoff ── STARTED/DESIGN_NOTIFIED → 소스 차단
                           IMPLEMENTING → 허용
                           미등록 → 전체 차단
```

---

## 4. 변경 파일 목록

| 영역 | 변경 | 파일 |
|------|------|------|
| DB 스키마 | RENAME + 신규 + 마이그레이션 v5 + MERGE_NOTIFIED 폐기 | `modules/handoff/module.go` |
| 상태 머신 | MERGE_NOTIFIED 제거, merge 전이 경로 제거 | `modules/handoff/state.go` |
| TxStore | Store에서 TxStore 분리 | `store/store.go`, `store/migrator.go` |
| TxStore 영향 | MigrateFunc + 전 모듈 콜백 시그니처 | `modules/*/module.go`, `modules/handoff/manager.go`, `modules/backlog/manage.go` |
| 테이블명 변경 | `branches` → `active_branches` 전체 SQL 쿼리 | `modules/handoff/manager.go` (~15쿼리), `modules/handoff/gate.go`, `modules/handoff/module.go` |
| notify start | --branch-name, 검증, 브랜치 생성 | `modules/handoff/manager.go`, `cli/handoff_cmd.go` |
| notify merge | NotifyTransition에서 분리 → NotifyMerge 신규 | `modules/handoff/manager.go`, `modules/handoff/module.go`, `cli/handoff_cmd.go` |
| notify drop | 신규 커맨드 | `modules/handoff/manager.go`, `modules/handoff/module.go`, `cli/handoff_cmd.go` |
| FIXING 게이트 | checkFixingBacklogs 공용 함수 추출 | `modules/handoff/manager.go`, `modules/handoff/gate.go` |
| notify merge/drop | Phase 2 git checkout main | `cli/handoff_cmd.go` |
| cleanup 연동 | active_branches 조회 + CWD 보호 | `cleanup/cleanup.go`, `cli/cleanup_cmd.go` |
| list-active | 신규 IPC 라우트 | `modules/handoff/module.go`, `modules/handoff/manager.go` |
| validate-build | git branch 생성 차단 패턴 확장 | `modules/hook/gate.go` |
| txs.Exec 버그 | 에러 체크 추가 | `modules/handoff/manager.go` |
| 백로그 | 5건 신규 등록 | `docs/BACKLOG.md` |
| 테스트 | 신규 로직 단위 + e2e | `cleanup/cleanup_test.go`, `e2e/` |

---

## 5. 백로그 등록 (이 PR 스코프 밖)

| 항목 | 등급 | 스코프 | 타입 |
|------|------|--------|------|
| `apex-agent git` CLI 서브커맨드 그룹 (checkout-main, switch, rebase) | MAJOR | TOOLS | INFRA |
| ModuleLogger `.With()` 매 호출 allocation 개선 | MINOR | TOOLS | PERF |
| Queue polling exponential backoff | MINOR | TOOLS | PERF |
| git 명령어 에러 핸들링 (rebase --abort 에러 무시) | MAJOR | TOOLS | BUG |
| Plugin 시스템 Claude Code 포맷 버전 체크 부재 | MINOR | TOOLS | INFRA |
