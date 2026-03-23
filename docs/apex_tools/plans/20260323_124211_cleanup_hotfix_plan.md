# cleanup 핫픽스 + 핸드오프 구조 강화 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** cleanup의 자기 워크트리 삭제 CRITICAL 버그를 수정하고, 핸드오프 기반 원자적 브랜치 생성/관리 체계를 구축한다.

**Architecture:** TxStore 타입 분리를 기반으로, DB 스키마(active_branches + branch_history)를 재구성하고, notify start/merge/drop 커맨드를 핸드오프 라이프사이클에 맞게 재설계. cleanup은 핸드오프 DB를 조회하여 활성 브랜치를 보호한다.

**Tech Stack:** Go 1.23, modernc.org/sqlite, cobra CLI, git exec

**스펙 문서:** `docs/apex_tools/plans/20260323_122926_cleanup_hotfix_spec.md`

---

## 파일 구조 (변경 맵)

```
apex_tools/apex-agent/
├── internal/
│   ├── store/
│   │   ├── store.go          # MODIFY: TxStore 분리, RunInTx 시그니처 변경
│   │   └── migrator.go       # MODIFY: MigrateFunc → func(*TxStore) error
│   ├── modules/
│   │   ├── handoff/
│   │   │   ├── manager.go    # MODIFY: NotifyMerge, NotifyDrop, checkFixingBacklogs, ListActive, table rename
│   │   │   ├── module.go     # MODIFY: migration v5, notify-merge/drop/list-active 라우트
│   │   │   ├── state.go      # MODIFY: MERGE_NOTIFIED 제거
│   │   │   └── gate.go       # MODIFY: ValidateMergeGate → checkFixingBacklogs 사용, table rename
│   │   ├── backlog/
│   │   │   ├── module.go     # MODIFY: MigrateFunc 시그니처
│   │   │   └── manage.go     # MODIFY: SetStatusWith 시그니처
│   │   ├── queue/
│   │   │   └── module.go     # MODIFY: MigrateFunc 시그니처
│   │   └── hook/
│   │       └── gate.go       # MODIFY: git branch 생성 차단 패턴
│   ├── cleanup/
│   │   ├── cleanup.go        # MODIFY: activeBranches 파라미터, CWD 보호
│   │   └── cleanup_test.go   # MODIFY: 신규 테스트
│   └── cli/
│       ├── handoff_cmd.go    # MODIFY: notify merge/drop/start 변경
│       └── cleanup_cmd.go    # MODIFY: list-active IPC 연동
├── e2e/
│   ├── handoff_test.go       # MODIFY: 신규 e2e 테스트
│   └── cleanup_test.go       # CREATE: cleanup e2e 테스트 (없다면)
└── docs/BACKLOG.md           # MODIFY: 5건 등록
```

---

## Task 1: TxStore 타입 분리

**Files:**
- Modify: `internal/store/store.go`
- Modify: `internal/store/migrator.go`

이 태스크는 기존 기능을 유지하면서 타입만 분리하는 리팩터링. 기존 테스트가 safety net.

- [ ] **Step 1: store.go에 TxStore 타입 정의 + Store 수정**

`Store`에서 `tx *sql.Tx` 필드 제거. `TxStore` 신규 타입 추가.
`RunInTx` 시그니처를 `func(tx *TxStore) error`로 변경.

```go
// TxStore — 트랜잭션 전용. RunInTx 콜백 안에서만 존재.
type TxStore struct {
    tx *sql.Tx
}

func (ts *TxStore) Exec(query string, args ...any) (sql.Result, error) {
    return ts.tx.Exec(query, args...)
}

func (ts *TxStore) Query(query string, args ...any) (*sql.Rows, error) {
    return ts.tx.Query(query, args...)
}

func (ts *TxStore) QueryRow(query string, args ...any) *sql.Row {
    return ts.tx.QueryRow(query, args...)
}

// Store — DB 전용. tx 필드 제거.
type Store struct {
    db *sql.DB
}

func (s *Store) RunInTx(ctx context.Context, fn func(tx *TxStore) error) error {
    tx, err := s.db.BeginTx(ctx, nil)
    if err != nil {
        return err
    }
    txs := &TxStore{tx: tx}
    if err := fn(txs); err != nil {
        tx.Rollback() //nolint:errcheck
        return err
    }
    return tx.Commit()
}
```

Store의 기존 `Exec/Query/QueryRow`는 `s.db` 직접 호출로 단순화 (tx 분기 제거).
`BeginTx` 메서드는 제거 (외부 호출자 없음 확인 완료 — `RunInTx` 내부에서만 사용).

- [ ] **Step 2: migrator.go의 MigrateFunc 시그니처 변경**

```go
type MigrateFunc func(tx *TxStore) error
```

`Migrator.Migrate()` 내 `RunInTx` 콜백도 `*TxStore` 사용:
```go
if err := m.store.RunInTx(context.Background(), func(tx *TxStore) error {
    if err := mig.fn(tx); err != nil {
        return fmt.Errorf("migrate %s v%d: %w", mig.module, mig.version, err)
    }
    if _, err := tx.Exec(
        "INSERT INTO _migrations (module, version) VALUES (?, ?)",
        mig.module, mig.version,
    ); err != nil {
        return err
    }
    return nil
}); err != nil {
```

- [ ] **Step 3: 전 모듈 콜백 시그니처 일괄 변경**

모든 `mig.Register("...", N, func(s *store.Store) error {` →
`mig.Register("...", N, func(tx *store.TxStore) error {`

콜백 내 `s.Exec` → `tx.Exec` 일괄 변경.

대상 파일:
- `modules/handoff/module.go`: v1~v4 (4개 콜백)
- `modules/backlog/module.go`: v1~v3 (3개 콜백)
- `modules/queue/module.go`: v1~v2 (2개 콜백)

- [ ] **Step 4: handoff manager.go의 RunInTx + SetStatusWith 시그니처 변경**

`NotifyStart`, `NotifyTransition` 등에서 `RunInTx(ctx, func(txs *Store)` → `func(tx *TxStore)`.

`BacklogOperator` 인터페이스:
```go
SetStatusWith(tx *store.TxStore, id int, status string) error
```

`backlog/manage.go`의 `SetStatusWith` 구현체도 시그니처 변경.

`manager.go`의 `getBacklogIDs(s *store.Store, ...)` → `getBacklogIDs(tx *store.TxStore, ...)`.

- [ ] **Step 5: 기존 테스트 실행하여 리팩터링 검증**

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: 기존 테스트 전부 PASS (타입 변경만, 동작 변경 없음)

- [ ] **Step 6: 커밋**

```bash
git add apex_tools/apex-agent/internal/
git commit -m "refactor(tools): TxStore 타입 분리 — Store/TxStore 책임 분리로 트랜잭션 오용 방지"
```

---

## Task 2: DB 스키마 마이그레이션 v5

**Files:**
- Modify: `internal/modules/handoff/module.go`

- [ ] **Step 1: migration v5 구현**

```go
mig.Register("handoff", 5, func(tx *store.TxStore) error {
    // 1. branches → active_branches 리네이밍
    if _, err := tx.Exec(`ALTER TABLE branches RENAME TO active_branches`); err != nil {
        return fmt.Errorf("rename branches: %w", err)
    }

    // 2. branch_history 테이블 생성
    if _, err := tx.Exec(`CREATE TABLE branch_history (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        branch      TEXT    NOT NULL,
        workspace   TEXT    NOT NULL,
        git_branch  TEXT,
        status      TEXT    NOT NULL,
        summary     TEXT,
        backlog_ids TEXT,
        started_at  TEXT    NOT NULL,
        finished_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
    )`); err != nil {
        return fmt.Errorf("create branch_history: %w", err)
    }

    // 3. MERGE_NOTIFIED → branch_history (MERGED) 이관
    if _, err := tx.Exec(`INSERT INTO branch_history (branch, workspace, git_branch, status, summary, started_at)
        SELECT branch, workspace, git_branch, 'MERGED', summary, created_at
        FROM active_branches WHERE status = 'MERGE_NOTIFIED'`); err != nil {
        return fmt.Errorf("migrate merge_notified: %w", err)
    }
    if _, err := tx.Exec(`DELETE FROM active_branches WHERE status = 'MERGE_NOTIFIED'`); err != nil {
        return fmt.Errorf("delete merge_notified: %w", err)
    }

    // 4. 나머지 전부 → branch_history (DROPPED) 이관
    if _, err := tx.Exec(`INSERT INTO branch_history (branch, workspace, git_branch, status, summary, started_at)
        SELECT branch, workspace, git_branch, 'DROPPED', summary, created_at
        FROM active_branches`); err != nil {
        return fmt.Errorf("migrate remaining: %w", err)
    }
    if _, err := tx.Exec(`DELETE FROM active_branches`); err != nil {
        return fmt.Errorf("delete remaining: %w", err)
    }

    // 5. git_branch UNIQUE 제약 추가 (테이블 재생성)
    if _, err := tx.Exec(`CREATE TABLE active_branches_v2 (
        branch      TEXT PRIMARY KEY,
        workspace   TEXT NOT NULL,
        git_branch  TEXT UNIQUE,
        status      TEXT NOT NULL,
        summary     TEXT,
        created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime')),
        updated_at  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
    )`); err != nil {
        return fmt.Errorf("create active_branches_v2: %w", err)
    }
    // active_branches는 이미 비어있으므로 데이터 이관 불필요
    if _, err := tx.Exec(`DROP TABLE active_branches`); err != nil {
        return fmt.Errorf("drop active_branches: %w", err)
    }
    if _, err := tx.Exec(`ALTER TABLE active_branches_v2 RENAME TO active_branches`); err != nil {
        return fmt.Errorf("rename active_branches_v2: %w", err)
    }

    // 6. 관련 junction 정리 (고아 레코드)
    if _, err := tx.Exec(`DELETE FROM branch_backlogs WHERE branch NOT IN (SELECT branch FROM active_branches)`); err != nil {
        return fmt.Errorf("cleanup branch_backlogs: %w", err)
    }

    return nil
})
```

- [ ] **Step 2: 테스트 실행** (마이그레이션 오류 확인)

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: 마이그레이션 관련 테스트 통과. 일부 핸드오프 테스트는 아직 `branches` 테이블명 참조로 실패할 수 있음 — Task 3에서 해결.

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/handoff/module.go
git commit -m "feat(tools): 핸드오프 DB 스키마 v5 — active_branches 리네이밍 + branch_history 신규"
```

---

## Task 3: 테이블명 + 상태 머신 변경

**Files:**
- Modify: `internal/modules/handoff/manager.go` — 모든 SQL `branches` → `active_branches`
- Modify: `internal/modules/handoff/gate.go` — SQL + MERGE_NOTIFIED 참조 제거
- Modify: `internal/modules/handoff/state.go` — MERGE_NOTIFIED 상수/전이 제거

- [ ] **Step 1: state.go 수정**

```go
// StatusMergeNotified 제거
const (
    StatusStarted        = "STARTED"
    StatusDesignNotified = "DESIGN_NOTIFIED"
    StatusImplementing   = "IMPLEMENTING"
)

// branch_history 전용 상태 (active에는 없음)
const (
    HistoryMerged  = "MERGED"
    HistoryDropped = "DROPPED"
)

// notification types — TypeMerge 제거 (notify-merge가 별도 핸들러)
const (
    TypeStart  = "start"
    TypeDesign = "design"
    TypePlan   = "plan"
    TypeMerge  = "merge" // 알림 타입으로는 유지 (notifications 테이블)
    TypeDrop   = "drop"  // 신규
)

// NextStatus — merge 전이 경로 제거
func NextStatus(current, notifyType string) (string, error) {
    switch {
    case current == StatusStarted && notifyType == TypeDesign:
        return StatusDesignNotified, nil
    case current == StatusDesignNotified && notifyType == TypePlan:
        return StatusImplementing, nil
    default:
        return "", fmt.Errorf("invalid transition: %s + %s", current, notifyType)
    }
}

// CanEditSource — MERGE_NOTIFIED 조건 제거
func CanEditSource(status string) bool {
    return status == StatusImplementing
}
```

- [ ] **Step 2: manager.go — `branches` → `active_branches` 일괄 치환**

manager.go 파일 전체에서 SQL 문자열 내 `branches` → `active_branches` 치환.
테이블명이 아닌 변수명/주석의 `branches`는 변경하지 않음.

대상 쿼리 (~15개):
- `SELECT status FROM branches WHERE branch = ?`
- `INSERT INTO branches (...)`
- `DELETE FROM branches WHERE branch = ?`
- `UPDATE branches SET status = ?`
- `SELECT branch, workspace, ... FROM branches WHERE branch = ?`
- `SELECT branch FROM branches WHERE git_branch = ?`
- `SELECT status FROM branches WHERE branch = ?` (GetStatus)
- 기타 참조

- [ ] **Step 3: gate.go — `branches` → `active_branches` + MERGE_NOTIFIED 참조 제거**

`ValidateMergeGate`의 SQL에서 `branches` → `active_branches` 변경.

**주의**: `BacklogCheck`는 gate.go가 아니라 **manager.go**에 있음 (line 287).
Step 2에서 manager.go의 `BacklogCheck`도 함께 수정:

```go
// manager.go BacklogCheck — active_branches에 있는 건 전부 활성이므로 상태 필터 불필요
// 기존: JOIN branches b ... WHERE bb.backlog_id = ? AND b.status != ?
// 변경: JOIN active_branches b ... WHERE bb.backlog_id = ?
row := m.store.QueryRow(
    `SELECT bb.branch FROM branch_backlogs bb
     JOIN active_branches b ON b.branch = bb.branch
     WHERE bb.backlog_id = ?`,
    backlogID,
)
```

- [ ] **Step 4: 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/handoff/
git commit -m "refactor(tools): branches→active_branches 일괄 변경 + MERGE_NOTIFIED 상태 폐기"
```

---

## Task 4: checkFixingBacklogs 공용 함수 + NotifyMerge/NotifyDrop

**Files:**
- Modify: `internal/modules/handoff/manager.go`
- Modify: `internal/modules/handoff/gate.go`
- Modify: `internal/modules/handoff/module.go`

- [ ] **Step 1: checkFixingBacklogs 함수 추출 (manager.go)**

`ValidateMergeGate`에서 FIXING 체크 로직을 추출:

```go
// checkFixingBacklogs returns FIXING backlog IDs linked to the given branch.
// Returns empty slice if no FIXING backlogs.
func (m *Manager) checkFixingBacklogs(branch string) ([]int, error) {
    if m.backlogManager == nil {
        return nil, nil
    }
    rows, err := m.store.Query(
        `SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch,
    )
    if err != nil {
        return nil, fmt.Errorf("query branch_backlogs: %w", err)
    }
    defer rows.Close()
    var backlogIDs []int
    for rows.Next() {
        var id int
        if scanErr := rows.Scan(&id); scanErr != nil {
            return nil, fmt.Errorf("scan branch_backlog id: %w", scanErr)
        }
        backlogIDs = append(backlogIDs, id)
    }
    if err := rows.Err(); err != nil {
        return nil, fmt.Errorf("iterate branch_backlogs: %w", err)
    }
    return m.backlogManager.ListFixingForBranch(branch, backlogIDs)
}
```

- [ ] **Step 2: ValidateMergeGate가 checkFixingBacklogs 사용하도록 변경 (gate.go)**

기존 FIXING 체크 인라인 코드를 `checkFixingBacklogs` 호출로 대체.

- [ ] **Step 3: finalizeBranch 헬퍼 함수 (manager.go)**

merge와 drop의 공통 DB TX 로직:

```go
// finalizeBranch moves an active branch to history and cleans up related records.
// historyStatus: HistoryMerged or HistoryDropped.
// notifType: TypeMerge or TypeDrop (notifications 테이블용, 기존 lowercase 규칙 유지).
func (m *Manager) finalizeBranch(branch, workspace, summary, historyStatus, notifType string) error {
    return m.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
        // 현재 브랜치 정보 조회
        var b Branch
        var dbSummary sql.NullString
        row := tx.QueryRow(
            `SELECT branch, workspace, COALESCE(git_branch,''), status, summary, created_at
             FROM active_branches WHERE branch = ?`, branch)
        if err := row.Scan(&b.Branch, &b.Workspace, &b.GitBranch, &b.Status, &dbSummary, &b.CreatedAt); err != nil {
            return fmt.Errorf("branch not found: %w", err)
        }

        // backlog IDs 스냅샷
        backlogIDs := m.getBacklogIDs(tx, branch)
        backlogJSON, _ := json.Marshal(backlogIDs)

        // branch_history 삽입
        if _, err := tx.Exec(
            `INSERT INTO branch_history (branch, workspace, git_branch, status, summary, backlog_ids, started_at)
             VALUES (?, ?, ?, ?, ?, ?, ?)`,
            b.Branch, b.Workspace, store.NullableString(b.GitBranch),
            historyStatus, dbSummary.String, string(backlogJSON), b.CreatedAt,
        ); err != nil {
            return fmt.Errorf("insert branch_history: %w", err)
        }

        // 관련 데이터 정리
        if _, err := tx.Exec(`DELETE FROM branch_backlogs WHERE branch = ?`, branch); err != nil {
            return fmt.Errorf("delete branch_backlogs: %w", err)
        }
        if _, err := tx.Exec(
            `DELETE FROM notification_acks WHERE branch = ? OR notification_id IN (SELECT id FROM notifications WHERE branch = ?)`,
            branch, branch); err != nil {
            return fmt.Errorf("delete notification_acks: %w", err)
        }
        if _, err := tx.Exec(`DELETE FROM notifications WHERE branch = ?`, branch); err != nil {
            return fmt.Errorf("delete notifications: %w", err)
        }
        if _, err := tx.Exec(`DELETE FROM active_branches WHERE branch = ?`, branch); err != nil {
            return fmt.Errorf("delete active_branches: %w", err)
        }

        // 알림 생성 (type은 lowercase 규칙 유지: "merge" or "drop")
        if _, err := tx.Exec(
            `INSERT INTO notifications (branch, workspace, type, summary, created_at)
             VALUES (?, ?, ?, ?, datetime('now','localtime'))`,
            branch, workspace, notifType, summary,
        ); err != nil {
            return fmt.Errorf("insert notification: %w", err)
        }

        return nil
    })
}
```

- [ ] **Step 4: NotifyMerge 함수 (manager.go)**

```go
func (m *Manager) NotifyMerge(branch, workspace, summary string) error {
    fixingIDs, err := m.checkFixingBacklogs(branch)
    if err != nil {
        return err
    }
    if len(fixingIDs) > 0 {
        return fmt.Errorf("FIXING 상태 백로그가 남아있습니다: %v\n먼저 backlog resolve 또는 release로 처리하세요", fixingIDs)
    }
    if err := m.finalizeBranch(branch, workspace, summary, HistoryMerged, TypeMerge); err != nil {
        return err
    }
    ml.Audit("branch merged", "branch", branch)
    return nil
}
```

- [ ] **Step 5: NotifyDrop 함수 (manager.go)**

```go
func (m *Manager) NotifyDrop(branch, workspace, reason string) error {
    fixingIDs, err := m.checkFixingBacklogs(branch)
    if err != nil {
        return err
    }
    if len(fixingIDs) > 0 {
        return fmt.Errorf("FIXING 상태 백로그가 남아있습니다: %v\n먼저 backlog release로 처리하세요", fixingIDs)
    }
    if err := m.finalizeBranch(branch, workspace, reason, HistoryDropped, TypeDrop); err != nil {
        return err
    }
    ml.Audit("branch dropped", "branch", branch, "reason", reason)
    return nil
}
```

- [ ] **Step 6: ListActive 함수 (manager.go)**

```go
type ActiveBranchInfo struct {
    Branch    string `json:"branch"`
    GitBranch string `json:"git_branch"`
}

func (m *Manager) ListActive() ([]ActiveBranchInfo, error) {
    rows, err := m.store.Query(`SELECT branch, COALESCE(git_branch,'') FROM active_branches`)
    if err != nil {
        return nil, err
    }
    defer rows.Close()
    var result []ActiveBranchInfo
    for rows.Next() {
        var info ActiveBranchInfo
        if err := rows.Scan(&info.Branch, &info.GitBranch); err != nil {
            return nil, err
        }
        result = append(result, info)
    }
    return result, rows.Err()
}
```

- [ ] **Step 7: module.go 라우트 등록 변경**

```go
reg.Handle("notify-merge", m.handleNotifyMerge)     // 신규
reg.Handle("notify-drop", m.handleNotifyDrop)         // 신규
reg.Handle("list-active", m.handleListActive)         // 신규
// notify-transition은 유지 (design, plan용)
```

핸들러 구현:
```go
type notifyMergeParams struct {
    Branch    string `json:"branch"`
    Workspace string `json:"workspace"`
    Summary   string `json:"summary"`
}

func (m *Module) handleNotifyMerge(_ context.Context, params json.RawMessage, _ string) (any, error) {
    var p notifyMergeParams
    if err := json.Unmarshal(params, &p); err != nil {
        return nil, fmt.Errorf("decode params: %w", err)
    }
    if err := m.manager.NotifyMerge(p.Branch, p.Workspace, p.Summary); err != nil {
        return nil, err
    }
    return map[string]string{"status": "merged"}, nil
}

type notifyDropParams struct {
    Branch    string `json:"branch"`
    Workspace string `json:"workspace"`
    Reason    string `json:"reason"`
}

func (m *Module) handleNotifyDrop(_ context.Context, params json.RawMessage, _ string) (any, error) {
    var p notifyDropParams
    if err := json.Unmarshal(params, &p); err != nil {
        return nil, fmt.Errorf("decode params: %w", err)
    }
    if err := m.manager.NotifyDrop(p.Branch, p.Workspace, p.Reason); err != nil {
        return nil, err
    }
    return map[string]string{"status": "dropped"}, nil
}

func (m *Module) handleListActive(_ context.Context, _ json.RawMessage, _ string) (any, error) {
    list, err := m.manager.ListActive()
    if err != nil {
        return nil, err
    }
    return map[string]any{"branches": list}, nil
}
```

- [ ] **Step 8: txs.Exec 에러 무시 버그 수정 (manager.go NotifyStart)**

기존 재등록 로직의 `txs.Exec(...)` 에러 무시 → 에러 체크 추가:
```go
// 기존 (에러 무시):
// txs.Exec(`DELETE FROM branch_backlogs WHERE branch = ?`, branch)
// 수정:
if _, err := tx.Exec(`DELETE FROM branch_backlogs WHERE branch = ?`, branch); err != nil {
    return fmt.Errorf("delete branch_backlogs: %w", err)
}
```
4개의 DELETE 문 모두 동일하게 수정.

- [ ] **Step 9: 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: PASS

- [ ] **Step 10: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/handoff/
git commit -m "feat(tools): NotifyMerge/NotifyDrop + checkFixingBacklogs 통합 + txs.Exec 에러 수정"
```

---

## Task 5: CLI — notify merge/drop 변경 + Phase 2 git checkout main

**Files:**
- Modify: `internal/cli/handoff_cmd.go`

- [ ] **Step 1: notify merge CLI — notify-merge IPC + Phase 2**

`handoffNotifyMergeCmd()` 수정:
```go
func handoffNotifyMergeCmd() *cobra.Command {
    var summary string
    cmd := &cobra.Command{
        Use:   "merge",
        Short: "머지 완료 알림",
        RunE: func(cmd *cobra.Command, args []string) error {
            branch := getBranchID()
            params := map[string]any{
                "branch":    branch,
                "workspace": branch,
                "summary":   summary,
            }
            // Phase 1: DB TX (IPC)
            _, err := sendHandoffRequest("notify-merge", params)
            if err != nil {
                return fmt.Errorf("daemon unavailable: %w", err)
            }
            fmt.Printf("[handoff] branch merged (branch=%s)\n", branch)

            // Phase 2: git checkout main
            if err := gitCheckoutMain(); err != nil {
                fmt.Fprintf(os.Stderr, "경고: git checkout main 실패: %v\n", err)
            }
            return nil
        },
    }
    cmd.Flags().StringVar(&summary, "summary", "", "머지 요약 (필수)")
    _ = cmd.MarkFlagRequired("summary")
    return cmd
}
```

- [ ] **Step 2: notify drop CLI — 신규 커맨드**

`handoffNotifyDropCmd()` 신규:
```go
func handoffNotifyDropCmd() *cobra.Command {
    var reason string
    cmd := &cobra.Command{
        Use:   "drop",
        Short: "작업 중도 포기",
        RunE: func(cmd *cobra.Command, args []string) error {
            branch := getBranchID()
            params := map[string]any{
                "branch":    branch,
                "workspace": branch,
                "reason":    reason,
            }
            _, err := sendHandoffRequest("notify-drop", params)
            if err != nil {
                return fmt.Errorf("daemon unavailable: %w", err)
            }
            fmt.Printf("[handoff] branch dropped (branch=%s, reason=%s)\n", branch, reason)

            if err := gitCheckoutMain(); err != nil {
                fmt.Fprintf(os.Stderr, "경고: git checkout main 실패: %v\n", err)
            }
            return nil
        },
    }
    cmd.Flags().StringVar(&reason, "reason", "", "포기 사유 (필수)")
    _ = cmd.MarkFlagRequired("reason")
    return cmd
}
```

- [ ] **Step 3: gitCheckoutMain 헬퍼 함수**

`ipc_helpers.go` 또는 `handoff_cmd.go` 하단에:
```go
func gitCheckoutMain() error {
    if _, err := exec.Command("git", "checkout", "main").Output(); err != nil {
        return fmt.Errorf("checkout main: %w", err)
    }
    if _, err := exec.Command("git", "pull", "origin", "main").Output(); err != nil {
        return fmt.Errorf("pull main: %w", err)
    }
    return nil
}
```

- [ ] **Step 4: handoffCmd()에 drop 등록**

```go
notify.AddCommand(handoffNotifyDropCmd())
```

- [ ] **Step 5: 빌드 확인**

Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`
Expected: 빌드 성공

- [ ] **Step 6: 커밋**

```bash
git add apex_tools/apex-agent/internal/cli/
git commit -m "feat(tools): notify merge/drop CLI — Phase 2 git checkout main 포함"
```

---

## Task 6: notify start — 브랜치 생성 + 검증 강화

**Files:**
- Modify: `internal/modules/handoff/manager.go`
- Modify: `internal/cli/handoff_cmd.go`

- [ ] **Step 1: NotifyStart 시그니처 + 브랜치명 검증 추가 (manager.go)**

**module.go의 notifyStartParams 수정**:
```go
type notifyStartParams struct {
    Branch     string `json:"branch"`
    Workspace  string `json:"workspace"`
    BranchName string `json:"branch_name"` // NEW: git 브랜치명
    Summary    string `json:"summary"`
    BacklogIDs []int  `json:"backlog_ids"`
    Scopes     string `json:"scopes"`
    SkipDesign bool   `json:"skip_design"`
}
// GitBranch 필드 제거 — BranchName이 git_branch 역할을 대체
// (브랜치가 아직 존재하지 않는 시점에 호출되므로 gitCurrentBranch() 무의미)
```

**handleNotifyStart 수정** — `p.BranchName`을 `gitBranch` 인자로 전달:
```go
id, err := m.manager.NotifyStart(p.Branch, p.Workspace, p.Summary, p.BranchName, p.BacklogIDs, p.Scopes, p.SkipDesign)
```

**manager.go — 검증 로직**:
```go
var branchNameRegex = regexp.MustCompile(`^(feature|bugfix)/[a-z0-9][a-z0-9_-]*$`)

func ValidateBranchName(name string) error {
    if !branchNameRegex.MatchString(name) {
        return fmt.Errorf("invalid branch name %q: must match (feature|bugfix)/[a-z0-9][a-z0-9_-]*", name)
    }
    return nil
}
```

`NotifyStart` 상단에 검증 추가:
```go
// 브랜치명 규칙 검증
if err := ValidateBranchName(gitBranch); err != nil {
    return 0, err
}
// DB 중복 체크 (git_branch UNIQUE)
var existingBranch string
row := m.store.QueryRow(`SELECT branch FROM active_branches WHERE git_branch = ?`, gitBranch)
if row.Scan(&existingBranch) == nil {
    return 0, fmt.Errorf("git 브랜치 '%s'가 이미 활성 작업 중 (workspace: %s)", gitBranch, existingBranch)
}
```

**주의**: `encoding/json`과 `regexp` import 추가 필요 (`json`은 `finalizeBranch`에서도 사용).

- [ ] **Step 2: CLI — --branch-name 플래그 + Phase 2 git 생성**

`handoffNotifyStartCmd()` 수정 — `--branch-name` 필수 플래그 추가.
**`gitCurrentBranch("")` 호출 제거** — 브랜치가 아직 존재하지 않으므로 의미 없음.
`git_branch` 파라미터 대신 `branch_name` 전송:

```go
var branchName string
// ...
cmd.Flags().StringVar(&branchName, "branch-name", "", "git 브랜치명 (필수, feature/* 또는 bugfix/*)")
_ = cmd.MarkFlagRequired("branch-name")
```

RunE에서:
```go
// Phase 0: git 브랜치 존재 확인 (로컬 + 리모트)
if err := exec.Command("git", "rev-parse", "--verify", "refs/heads/"+branchName).Run(); err == nil {
    return fmt.Errorf("로컬 git 브랜치 '%s'가 이미 존재합니다", branchName)
}
if out, _ := exec.Command("git", "ls-remote", "--heads", "origin", branchName).Output(); len(strings.TrimSpace(string(out))) > 0 {
    return fmt.Errorf("리모트 git 브랜치 'origin/%s'가 이미 존재합니다", branchName)
}

// Phase 1: DB TX (IPC)
params := map[string]any{
    "branch":      branch,
    "workspace":   branch,
    "branch_name": branchName,  // git_branch 대신 branch_name
    "summary":     summary,
    "backlog_ids": backlogs,
    "scopes":      scopes,
    "skip_design": skipDesign,
}
result, err := sendHandoffRequest("notify-start", params)
if err != nil {
    return fmt.Errorf("daemon unavailable: %w", err)
}

// Phase 2: Git branch creation (DB 레코드가 보호 — 실패 시 재시도 가능)
if err := exec.Command("git", "checkout", "-b", branchName).Run(); err != nil {
    return fmt.Errorf("git checkout -b 실패: %w (DB 레코드는 생성됨 — 재시도 가능)", err)
}
if err := exec.Command("git", "push", "-u", "origin", branchName).Run(); err != nil {
    fmt.Fprintf(os.Stderr, "경고: git push 실패: %v (재시도 필요)\n", err)
}
```

**주의**: `os/exec`와 `strings` import 추가 필요.

`handoffNotifyStartJobCmd()`에도 동일하게 `--branch-name` + Phase 0/2 추가.
`--backlog` 플래그는 `job` 서브커맨드에 정의되지 않으므로 cobra가 자동 거부 — 별도 검증 불필요.

- [ ] **Step 3: --scopes 필수로 격상**

`cmd.MarkFlagRequired("scopes")` 양쪽 모두 추가.

- [ ] **Step 4: 빌드 확인**

Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`
Expected: 빌드 성공

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/
git commit -m "feat(tools): notify start — --branch-name 필수화 + Phase 2 git 브랜치 생성"
```

---

## Task 7: cleanup 연동 + CWD 보호

**Files:**
- Modify: `internal/cleanup/cleanup.go`
- Modify: `internal/cleanup/cleanup_test.go`
- Modify: `internal/cli/cleanup_cmd.go`

- [ ] **Step 1: cleanup_test.go — 신규 테스트 작성**

```go
func TestActiveBranchesSkipped(t *testing.T) {
    // Setup: activeBranches map에 "feature/test" 포함
    // Run과 같은 로직으로 로컬 브랜치 처리 시 스킵 확인
    // (실제 git 호출 없이 로직만 검증)
}

func TestIsSubPath(t *testing.T) {
    cases := []struct {
        parent, child string
        want          bool
    }{
        {"/workspace/repo", "/workspace/repo/src", true},
        {"/workspace/repo", "/workspace/other", false},
        {"/workspace/repo", "/workspace/repo", true},
    }
    for _, tc := range cases {
        got := isSubPath(tc.child, tc.parent)
        if got != tc.want {
            t.Errorf("isSubPath(%q, %q) = %v, want %v", tc.child, tc.parent, got, tc.want)
        }
    }
}
```

- [ ] **Step 2: cleanup.go — isSubPath 함수**

```go
func isSubPath(child, parent string) bool {
    absChild, err1 := filepath.Abs(child)
    absParent, err2 := filepath.Abs(parent)
    if err1 != nil || err2 != nil {
        return false
    }
    // Windows: 대소문자 무시
    if runtime.GOOS == "windows" {
        absChild = strings.ToLower(absChild)
        absParent = strings.ToLower(absParent)
    }
    rel, err := filepath.Rel(absParent, absChild)
    if err != nil {
        return false
    }
    return !strings.HasPrefix(rel, "..")
}
```

- [ ] **Step 3: cleanup.go — Run() 시그니처 변경 + 핸드오프 체크**

```go
func Run(repoRoot string, execute bool, activeBranches map[string]bool) (*Result, error)
```

`processWorktrees`에 CWD 보호 + activeBranches 체크:
```go
func processWorktrees(repoRoot string, execute bool, activeBranches map[string]bool, result *Result) (map[string]bool, error) {
    cwd, _ := os.Getwd()
    // ... 기존 루프 내에:
    // CWD 보호
    if isSubPath(cwd, wtPath) {
        result.Warnings = append(result.Warnings,
            fmt.Sprintf("현재 작업 디렉토리: %s [%s] — 스킵", wtPath, wtBranch))
        continue
    }
    // 핸드오프 보호
    if activeBranches[wtBranch] {
        result.Warnings = append(result.Warnings,
            fmt.Sprintf("활성 핸드오프: %s [%s] — 스킵", wtPath, wtBranch))
        continue
    }
```

`processLocalBranches`, `processRemoteBranches`에도 동일하게 `activeBranches` 체크 추가.

- [ ] **Step 4: cleanup_cmd.go — list-active IPC 연동**

```go
RunE: func(cmd *cobra.Command, args []string) error {
    // 핸드오프 활성 브랜치 조회
    activeBranches := map[string]bool{}
    activeResult, listErr := sendHandoffRequest("list-active", nil)
    if listErr == nil {
        if branches, ok := activeResult["branches"].([]any); ok {
            for _, raw := range branches {
                data, _ := json.Marshal(raw)
                var info struct {
                    GitBranch string `json:"git_branch"`
                }
                if json.Unmarshal(data, &info) == nil && info.GitBranch != "" {
                    activeBranches[info.GitBranch] = true
                }
            }
        }
    }
    // 데몬 불가 시 빈 맵으로 진행 (CWD 보호만 작동)

    cleanupResult, err := cleanup.Run(cwd, execute, activeBranches)
    // ...
}
```

- [ ] **Step 5: 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./internal/cleanup/ -count=1 -v`
Expected: PASS

- [ ] **Step 6: 커밋**

```bash
git add apex_tools/apex-agent/internal/cleanup/ apex_tools/apex-agent/internal/cli/cleanup_cmd.go
git commit -m "fix(tools): cleanup — 핸드오프 연동 + CWD 보호로 자기 워크트리 삭제 방지"
```

---

## Task 8: validate-build hook — git 브랜치 생성 차단

**Files:**
- Modify: `internal/modules/hook/gate.go`
- Modify: `internal/cleanup/cleanup_test.go` (또는 `internal/modules/hook/gate_test.go`)

- [ ] **Step 1: gate_test.go — 차단 패턴 테스트 작성**

```go
func TestValidateBuildBlocksGitBranchCreation(t *testing.T) {
    blocked := []string{
        "git checkout -b feature/foo",
        "git checkout -B feature/foo",
        "git switch -c feature/foo",
        "git switch --create feature/foo",
        "git branch feature/foo",
        "git worktree add -b feature/foo ../worktree",
    }
    for _, cmd := range blocked {
        if err := ValidateBuild(cmd); err == nil {
            t.Errorf("expected block for %q", cmd)
        }
    }

    allowed := []string{
        "git branch",
        "git branch -a",
        "git branch -v",
        "git branch --list",
        "git branch -D feature/foo",
        "git branch -d feature/foo",
        "git checkout main",
        "git checkout feature/existing",
        "git status",
        "git push origin --delete feature/foo",
    }
    for _, cmd := range allowed {
        if err := ValidateBuild(cmd); err != nil {
            t.Errorf("unexpected block for %q: %v", cmd, err)
        }
    }
}
```

- [ ] **Step 2: gate.go — isBlockedGitBranch 함수 + 패턴 추가**

```go
var blockedGitBranchPatterns = []*regexp.Regexp{
    regexp.MustCompile(`\bgit\s+checkout\s+-[bB]\b`),
    regexp.MustCompile(`\bgit\s+switch\s+(-c|--create)\b`),
    regexp.MustCompile(`\bgit\s+worktree\s+add\s+.*-b\b`),
}

func isBlockedGitBranch(command string) bool {
    for _, pat := range blockedGitBranchPatterns {
        if pat.MatchString(command) {
            return true
        }
    }
    // git branch <name> (플래그 없는 인자 = 브랜치 생성)
    return isGitBranchCreate(command)
}

func isGitBranchCreate(command string) bool {
    // "git branch" 뒤에 플래그가 아닌 인자가 있으면 브랜치 생성
    fields := strings.Fields(command)
    for i, f := range fields {
        if f == "git" && i+1 < len(fields) && fields[i+1] == "branch" {
            // git branch 뒤의 토큰 검사
            for _, arg := range fields[i+2:] {
                if strings.HasPrefix(arg, "-") {
                    return false // 플래그가 있으면 조회/삭제 커맨드
                }
                return true // 플래그 없는 인자 = 브랜치 생성
            }
            return false // git branch (인자 없음) = 조회
        }
    }
    return false
}
```

`ValidateBuild` 함수에 체크 추가:
```go
// Check git branch creation (after apex-agent/go allow).
if isBlockedGitBranch(command) {
    return fmt.Errorf("차단: 직접 브랜치 생성 금지. 'apex-agent handoff notify start --branch-name <name>' 을 사용하세요")
}
```

- [ ] **Step 3: 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./internal/modules/hook/ -count=1 -v`
Expected: PASS

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/hook/
git commit -m "feat(tools): validate-build — git 브랜치 생성 명령 차단 (notify start 경유 강제)"
```

---

## Task 9: 백로그 등록 + 문서 갱신

**Files:**
- Modify: `docs/BACKLOG.md`

- [ ] **Step 1: 다음 발번 확인 후 5건 등록**

현재 다음 발번: 154. 154~158 사용.

```markdown
### #154. apex-agent git CLI 서브커맨드 그룹
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: INFRA
- **설명**: apex-agent에 `git` 서브커맨드 그룹 추가 (checkout-main, switch, rebase). validate-build hook이 git 브랜치 생성을 차단하므로, 데몬 기반 안전한 git 조작 경로 필요. 나중에 핸드오프 검증 연동(switch 시 해당 브랜치의 handoff 상태 확인) 가능.

### #155. ModuleLogger .With() 매 호출 allocation 개선
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: PERF
- **설명**: ModuleLogger.Debug/Info/Warn/Error가 매 호출마다 slog.Default().With("module", ml.name)으로 새 Logger를 생성. hot path 아니지만 sync.Once 기반 lazy init 또는 slog.LogAttrs로 allocation 절감 가능.

### #156. Queue polling exponential backoff
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: PERF
- **설명**: Queue.Acquire에서 락 획득 실패 시 500ms 고정 sleep. exponential backoff(100ms→200ms→400ms→max 2s) 또는 context.WithTimeout 전체 대기 시간 제한 추가.

### #157. git 명령어 에러 핸들링 — rebase abort 에러 무시
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: EnforceRebase에서 rebase 실패 시 --abort 호출의 에러를 무시(//nolint:errcheck). abort 자체가 실패하면(dirty working tree 등) 반쪽짜리 rebase 상태에 빠질 수 있음. 최소 경고 로그 기록 필요.

### #158. Plugin 시스템 Claude Code 포맷 버전 체크 부재
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: INFRA
- **설명**: plugin.Setup()이 ~/.claude/ 디렉토리 구조, installed_plugins.json, settings.json 포맷에 직접 의존. 포맷 변경 시 무조건 깨짐. 파일 포맷 버전 체크를 추가하여 호환성 깨질 때 명시적 에러 반환.
```

다음 발번 → 159로 갱신.

- [ ] **Step 2: 커밋**

```bash
git add docs/BACKLOG.md
git commit -m "docs(backlog): 5건 등록 — git CLI, ModuleLogger, Queue polling, rebase abort, Plugin 버전"
```

---

## Task 10: 통합 테스트 + 빌드 검증

**Files:**
- Modify: `e2e/handoff_test.go`
- Modify: `e2e/cleanup_test.go` (있다면)

- [ ] **Step 1: e2e 테스트 — notify merge/drop 시나리오**

기존 handoff e2e 테스트에 추가:
- notify start → notify merge → active_branches 비어있음 + branch_history에 MERGED 기록
- notify start → notify drop → FIXING 잔존 시 에러 → release 후 재시도 → 성공
- list-active 라우트 응답 검증

- [ ] **Step 2: e2e 테스트 — cleanup 연동**

- activeBranches에 포함된 브랜치가 cleanup에서 스킵되는지 검증

- [ ] **Step 3: 전체 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./... -count=1 -v`
Expected: 전체 PASS

- [ ] **Step 4: 바이너리 빌드 + 설치**

Run:
```bash
cd apex_tools/apex-agent
go build -o apex-agent.exe ./cmd/apex-agent
apex-agent daemon stop 2>/dev/null || true
cp apex-agent.exe "$LOCALAPPDATA/apex-agent/apex-agent.exe"
apex-agent daemon start
```

- [ ] **Step 5: 수동 검증 — 핸드오프 재등록**

마이그레이션으로 기존 레코드가 정리되었으므로, 현재 브랜치의 핸드오프를 재등록:
```bash
apex-agent handoff notify start job \
  --branch-name bugfix/cleanup-self-destruct \
  --scopes tools \
  --summary "cleanup 핫픽스 + 핸드오프 구조 강화" \
  --skip-design
```

- [ ] **Step 6: 최종 커밋**

```bash
git add apex_tools/apex-agent/e2e/
git commit -m "test(tools): 핸드오프 merge/drop + cleanup 연동 e2e 테스트"
```

---

## 실행 순서 요약

| Task | 의존 | 설명 |
|------|------|------|
| 1 | - | TxStore 타입 분리 (기반) |
| 2 | 1 | DB 스키마 마이그레이션 v5 |
| 3 | 2 | 테이블명 + 상태 머신 변경 |
| 4 | 3 | checkFixingBacklogs + NotifyMerge/Drop + ListActive |
| 5 | 4 | CLI merge/drop + Phase 2 |
| 6 | 5 | notify start 브랜치 생성 |
| 7 | 4 | cleanup 연동 + CWD 보호 |
| 8 | - | validate-build git 차단 (독립) |
| 9 | - | 백로그 등록 (독립) |
| 10 | 1~9 | 통합 테스트 + 빌드 |

Task 7과 8은 Task 4 이후 병렬 가능. Task 9는 독립.
