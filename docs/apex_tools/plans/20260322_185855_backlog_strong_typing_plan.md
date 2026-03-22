# 백로그 강타입 + 핸드오프 연동 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 백로그 열거형 필드에 코드 레벨 타입 강제 + 핸드오프 시스템과 백로그 연동 체계화 (junction 테이블, FIXING 상태, 머지 시 강제 해결)

**Architecture:** enums.go(검증 함수) → manage.go/import.go/export.go(대문자 전환) → handoff migration(junction 테이블) → manager.go(NotifyStart 복수 백로그) → gate.go(머지 시 FIXING 차단) → CLI(start job 분리 + release 커맨드)

**Tech Stack:** Go, SQLite, cobra CLI

**Spec:** `docs/apex_tools/plans/20260322_182401_backlog_strong_typing_spec.md`

**주의:** 모든 Go 소스 파일 첫 줄에 MIT 라이선스 헤더 필수:
`// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.`

---

## File Map

모든 경로는 `apex_tools/apex-agent/` 기준.

### 생성 파일

| 파일 | 책임 | Task |
|------|------|:----:|
| `internal/modules/backlog/enums.go` | const 정의 + Validate 함수 | 1 |

### 수정 파일

| 파일 | 변경 내용 | Task |
|------|-----------|:----:|
| `internal/modules/backlog/manage.go` | Add/Resolve 검증, Release(), SetStatus(), 하드코딩 문자열→const | 2 |
| `internal/modules/backlog/module.go` | 마이그레이션 v3 (대문자+DEFAULT) + release 라우트 | 2 |
| `internal/modules/backlog/import.go` | import 시 대문자 변환 | 3 |
| `internal/modules/backlog/export.go` | export 시 Status 필터 const 사용 | 3 |
| `internal/modules/handoff/module.go` | 마이그레이션 v2 (branch_backlogs junction + branches 재생성) | 4 |
| `internal/modules/handoff/manager.go` | NotifyStart 시그니처 변경, junction INSERT, FIXING 전이, BacklogCheck/GetBranch junction 대응 | 5 |
| `internal/modules/handoff/gate.go` | ValidateMergeGate에 FIXING 백로그 차단 추가 | 5 |
| `internal/cli/handoff_cmd.go` | start job 서브커맨드, --backlog IntSlice, handoff status 복수 백로그 표시 | 6 |
| `internal/cli/backlog_cmd.go` | release 커맨드, resolve 도움말 수정, list 필터 기본값 OPEN | 6 |
| `internal/modules/backlog/*_test.go` | 하드코딩 문자열 → const 전환 | 7 |
| `internal/modules/handoff/*_test.go` | junction 기반 테스트 갱신 | 7 |
| `e2e/backlog_test.go` | 대문자 enum + release 시나리오 | 7 |
| `e2e/handoff_test.go` | backlogIDs 슬라이스 + job 모드 | 7 |
| `docs/CLAUDE.md` | 스코프/타입 태그 UPPER_SNAKE_CASE | 8 |

---

## 의존성 그래프

```
Task 1 (enums.go)
  ├→ Task 2 (manage.go + module.go)
  │    └→ Task 3 (import.go + export.go)
  │
  └→ Task 4 (handoff migration v2) [독립]
       └→ Task 5 (handoff manager + gate)
            └→ Task 6 (CLI 변경)
                 └→ Task 7 (테스트 갱신)
                      └→ Task 8 (문서)
```

Tasks 1~3 (backlog)과 Task 4 (handoff migration)는 독립 — 병렬 가능.
Task 5는 Task 2 + Task 4 모두 필요.

---

## Task 1: Enums 패키지

**Files:**
- Create: `internal/modules/backlog/enums.go`

- [ ] **Step 1: enums.go 작성**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"fmt"
	"strings"
)

// --- Severity ---

const (
	SeverityCritical = "CRITICAL"
	SeverityMajor    = "MAJOR"
	SeverityMinor    = "MINOR"
)

var validSeverities = map[string]bool{
	SeverityCritical: true, SeverityMajor: true, SeverityMinor: true,
}

func ValidateSeverity(s string) error {
	if !validSeverities[s] {
		return fmt.Errorf("invalid severity %q: must be CRITICAL|MAJOR|MINOR", s)
	}
	return nil
}

// --- Timeframe ---

const (
	TimeframeNow      = "NOW"
	TimeframeInView   = "IN_VIEW"
	TimeframeDeferred = "DEFERRED"
)

var validTimeframes = map[string]bool{
	TimeframeNow: true, TimeframeInView: true, TimeframeDeferred: true,
	"": true, // 히스토리 import용
}

func ValidateTimeframe(s string) error {
	if !validTimeframes[s] {
		return fmt.Errorf("invalid timeframe %q: must be NOW|IN_VIEW|DEFERRED", s)
	}
	return nil
}

// --- Type ---

const (
	TypeBug        = "BUG"
	TypeDesignDebt = "DESIGN_DEBT"
	TypeDocs       = "DOCS"
	TypeInfra      = "INFRA"
	TypePerf       = "PERF"
	TypeSecurity   = "SECURITY"
	TypeTest       = "TEST"
)

var validTypes = map[string]bool{
	TypeBug: true, TypeDesignDebt: true, TypeDocs: true, TypeInfra: true,
	TypePerf: true, TypeSecurity: true, TypeTest: true,
}

func ValidateType(s string) error {
	if !validTypes[s] {
		return fmt.Errorf("invalid type %q: must be BUG|DESIGN_DEBT|DOCS|INFRA|PERF|SECURITY|TEST", s)
	}
	return nil
}

// --- Status ---

const (
	StatusOpen     = "OPEN"
	StatusFixing   = "FIXING"
	StatusResolved = "RESOLVED"
)

var validStatuses = map[string]bool{
	StatusOpen: true, StatusFixing: true, StatusResolved: true,
}

func ValidateStatus(s string) error {
	if !validStatuses[s] {
		return fmt.Errorf("invalid status %q: must be OPEN|FIXING|RESOLVED", s)
	}
	return nil
}

// --- Resolution ---

const (
	ResolutionFixed      = "FIXED"
	ResolutionDocumented = "DOCUMENTED"
	ResolutionWontfix    = "WONTFIX"
	ResolutionDuplicate  = "DUPLICATE"
	ResolutionSuperseded = "SUPERSEDED"
)

var validResolutions = map[string]bool{
	ResolutionFixed: true, ResolutionDocumented: true, ResolutionWontfix: true,
	ResolutionDuplicate: true, ResolutionSuperseded: true,
}

func ValidateResolution(s string) error {
	if !validResolutions[s] {
		return fmt.Errorf("invalid resolution %q: must be FIXED|DOCUMENTED|WONTFIX|DUPLICATE|SUPERSEDED", s)
	}
	return nil
}

// --- Scope ---

const (
	ScopeCore    = "CORE"
	ScopeShared  = "SHARED"
	ScopeGateway = "GATEWAY"
	ScopeAuthSvc = "AUTH_SVC"
	ScopeChatSvc = "CHAT_SVC"
	ScopeInfra   = "INFRA"
	ScopeCI      = "CI"
	ScopeDocs    = "DOCS"
	ScopeTools   = "TOOLS"
)

var validScopes = map[string]bool{
	ScopeCore: true, ScopeShared: true, ScopeGateway: true,
	ScopeAuthSvc: true, ScopeChatSvc: true, ScopeInfra: true,
	ScopeCI: true, ScopeDocs: true, ScopeTools: true,
}

// ValidateScope validates a comma-separated scope string.
// Each token is trimmed and checked individually.
func ValidateScope(s string) error {
	if s == "" {
		return fmt.Errorf("scope must not be empty")
	}
	for _, part := range strings.Split(s, ",") {
		token := strings.TrimSpace(part)
		if token == "" {
			continue
		}
		if !validScopes[token] {
			return fmt.Errorf("invalid scope %q: must be CORE|SHARED|GATEWAY|AUTH_SVC|CHAT_SVC|INFRA|CI|DOCS|TOOLS", token)
		}
	}
	return nil
}

// NormalizeScope converts legacy scope formats to UPPER_SNAKE_CASE.
// "core, shared" → "CORE, SHARED", "auth-svc" → "AUTH_SVC"
func NormalizeScope(s string) string {
	parts := strings.Split(s, ",")
	for i, p := range parts {
		p = strings.TrimSpace(p)
		p = strings.ToUpper(p)
		p = strings.ReplaceAll(p, "-", "_")
		parts[i] = p
	}
	return strings.Join(parts, ", ")
}

// NormalizeType converts legacy type formats to UPPER_SNAKE_CASE.
// "bug" → "BUG", "design-debt" → "DESIGN_DEBT"
func NormalizeType(s string) string {
	s = strings.ToUpper(s)
	s = strings.ReplaceAll(s, "-", "_")
	return s
}

// NormalizeStatus converts legacy status formats.
// "open" → "OPEN", "resolved" → "RESOLVED"
func NormalizeStatus(s string) string {
	return strings.ToUpper(s)
}

// NormalizeResolution cleans up resolution values.
// Handles corrupted data like "WONTFIX → **정정: FIXED (v0.5.10.2)**" → "FIXED"
func NormalizeResolution(s string) string {
	s = strings.TrimSpace(s)
	// 오염 데이터: "WONTFIX → ..." 패턴 → 마지막 유효 resolution 추출
	if strings.Contains(s, "→") || strings.Contains(s, "정정") {
		for _, res := range []string{"FIXED", "DOCUMENTED", "WONTFIX", "DUPLICATE", "SUPERSEDED"} {
			// 마지막으로 등장하는 유효 resolution을 사용
			if strings.Contains(strings.ToUpper(s), res) {
				// "정정: FIXED" 패턴이면 FIXED 사용
				if strings.Contains(s, "정정") {
					idx := strings.LastIndex(strings.ToUpper(s), res)
					if idx >= 0 {
						return res
					}
				}
			}
		}
	}
	return strings.ToUpper(s)
}
```

- [ ] **Step 2: 빌드 확인**

Run: `go build ./cmd/apex-agent`
Expected: 빌드 성공 (enums.go는 독립 패키지, 기존 코드 영향 없음)

- [ ] **Step 3: 커밋**

```bash
git add internal/modules/backlog/enums.go
git commit -m "feat(tools): BACKLOG-126 백로그 열거형 const + 검증 함수"
git push
```

---

## Task 2: Backlog manage.go + module.go 갱신

**Files:**
- Modify: `internal/modules/backlog/manage.go`
- Modify: `internal/modules/backlog/module.go`

**Ref:** 스펙 §2 열거형 검증, §4 마이그레이션 v3, §5 release

- [ ] **Step 1: manage.go — Add()에 검증 추가 + 하드코딩 문자열 const 전환**

`Add()` 함수 맨 앞에 검증 호출 추가:
```go
if err := ValidateSeverity(item.Severity); err != nil {
    return err
}
if err := ValidateTimeframe(item.Timeframe); err != nil {
    return err
}
if err := ValidateType(item.Type); err != nil {
    return err
}
if err := ValidateScope(item.Scope); err != nil {
    return err
}
```

`status` 기본값: `"open"` → `StatusOpen`
`List()` 기본값: `"open"` → `StatusOpen`
`Resolve()`: `'resolved'` → `StatusResolved`

- [ ] **Step 2: manage.go — Resolve()에 Resolution 검증 추가**

```go
func (m *Manager) Resolve(id int, resolution string) error {
    if err := ValidateResolution(resolution); err != nil {
        return err
    }
    // ... 기존 로직, 단 'resolved' → StatusResolved
```

- [ ] **Step 3: manage.go — Release() + SetStatus() 추가**

```go
// SetStatus updates the status of a backlog item.
func (m *Manager) SetStatus(id int, status string) error {
	if err := ValidateStatus(status); err != nil {
		return err
	}
	result, err := m.store.Exec(`
		UPDATE backlog_items SET status = ?, updated_at = datetime('now','localtime') WHERE id = ?`,
		status, id,
	)
	if err != nil {
		return fmt.Errorf("SetStatus: %w", err)
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		return fmt.Errorf("SetStatus: item %d not found", id)
	}
	ml.Info("status changed", "id", id, "status", status)
	return nil
}

// Release removes a backlog item from active work.
// If status is FIXING, sets it back to OPEN.
// Appends release reason to description.
func (m *Manager) Release(id int, reason, branch string) error {
	item, err := m.Get(id)
	if err != nil {
		return fmt.Errorf("Release: %w", err)
	}
	if item == nil {
		return fmt.Errorf("Release: item %d not found", id)
	}

	// Append release history to description
	timestamp := "" // datetime('now','localtime') in SQL
	appendDesc := fmt.Sprintf("\n[RELEASED] %s: %s", branch, reason)

	_, err = m.store.Exec(`
		UPDATE backlog_items
		SET status = CASE WHEN status = ? THEN ? ELSE status END,
		    description = description || ?,
		    updated_at = datetime('now','localtime')
		WHERE id = ?`,
		StatusFixing, StatusOpen, appendDesc, id,
	)
	if err != nil {
		return fmt.Errorf("Release: %w", err)
	}

	// Remove from branch_backlogs (cross-table, same DB)
	_, _ = m.store.Exec(`DELETE FROM branch_backlogs WHERE backlog_id = ?`, id)

	ml.Info("item released", "id", id, "reason", reason)
	return nil
}
```

- [ ] **Step 4: module.go — 마이그레이션 v3 + release 라우트**

v3 마이그레이션: 대문자 정규화 + DEFAULT 'OPEN' 스키마 재생성.

```go
mig.Register("backlog", 3, func(s *store.Store) error {
    // 1. 기존 데이터 대문자 정규화
    updates := []string{
        `UPDATE backlog_items SET status = UPPER(status) WHERE status != UPPER(status)`,
        `UPDATE backlog_items SET severity = UPPER(severity) WHERE severity != UPPER(severity)`,
        `UPDATE backlog_items SET type = UPPER(REPLACE(type, '-', '_')) WHERE type != UPPER(REPLACE(type, '-', '_'))`,
    }
    for _, q := range updates {
        if _, err := s.Exec(q); err != nil {
            return fmt.Errorf("normalize: %w", err)
        }
    }

    // scope: 쉼표 구분 각각 정규화 (Go에서 처리)
    rows, err := s.Query("SELECT id, scope FROM backlog_items")
    if err != nil {
        return err
    }
    defer rows.Close()
    type idScope struct{ id int; scope string }
    var pairs []idScope
    for rows.Next() {
        var p idScope
        rows.Scan(&p.id, &p.scope)
        pairs = append(pairs, p)
    }
    rows.Close()
    for _, p := range pairs {
        normalized := NormalizeScope(p.scope)
        if normalized != p.scope {
            s.Exec("UPDATE backlog_items SET scope = ? WHERE id = ?", normalized, p.id)
        }
    }

    // resolution 오염 데이터 정리
    resRows, err := s.Query("SELECT id, resolution FROM backlog_items WHERE resolution IS NOT NULL")
    if err != nil {
        return err
    }
    defer resRows.Close()
    type idRes struct{ id int; res string }
    var resPairs []idRes
    for resRows.Next() {
        var p idRes
        resRows.Scan(&p.id, &p.res)
        resPairs = append(resPairs, p)
    }
    resRows.Close()
    for _, p := range resPairs {
        normalized := NormalizeResolution(p.res)
        if normalized != p.res {
            s.Exec("UPDATE backlog_items SET resolution = ? WHERE id = ?", normalized, p.id)
        }
    }

    // 2. 스키마 재생성 (DEFAULT 'OPEN')
    _, err = s.Exec(`
        CREATE TABLE backlog_items_v3 (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            title       TEXT    NOT NULL,
            severity    TEXT    NOT NULL,
            timeframe   TEXT    NOT NULL,
            scope       TEXT    NOT NULL,
            type        TEXT    NOT NULL,
            description TEXT    NOT NULL,
            related     TEXT,
            position    INTEGER NOT NULL,
            status      TEXT    NOT NULL DEFAULT 'OPEN',
            resolution  TEXT,
            resolved_at TEXT,
            created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
        );
        INSERT INTO backlog_items_v3 SELECT * FROM backlog_items;
        DROP TABLE backlog_items;
        ALTER TABLE backlog_items_v3 RENAME TO backlog_items;
    `)
    return err
})
```

release 라우트 추가: `reg.Handle("release", m.handleRelease)`

```go
func (m *Module) handleRelease(_ context.Context, params json.RawMessage, _ string) (any, error) {
    var p struct {
        ID     int    `json:"id"`
        Reason string `json:"reason"`
        Branch string `json:"branch"`
    }
    if err := json.Unmarshal(params, &p); err != nil {
        return nil, fmt.Errorf("backlog.release: decode params: %w", err)
    }
    if err := m.manager.Release(p.ID, p.Reason, p.Branch); err != nil {
        return nil, err
    }
    return map[string]string{"status": "released"}, nil
}
```

- [ ] **Step 5: 빌드 + 테스트**

Run: `go build ./cmd/apex-agent && go test ./internal/modules/backlog/... -v`
Expected: 빌드 성공. 테스트는 하드코딩 문자열 불일치로 일부 FAIL 예상 (Task 7에서 수정).

- [ ] **Step 6: 커밋**

```bash
git add internal/modules/backlog/manage.go internal/modules/backlog/module.go
git commit -m "feat(tools): BACKLOG-126 백로그 검증 + Release/SetStatus + 마이그레이션 v3"
git push
```

---

## Task 3: import.go + export.go 대문자 전환

**Files:**
- Modify: `internal/modules/backlog/import.go`
- Modify: `internal/modules/backlog/export.go`

- [ ] **Step 1: import.go — 파싱 시 대문자 변환**

`ParseBacklogMD()`:
- `Status: "open"` → `Status: StatusOpen`
- scope, type 필드: `NormalizeScope()`, `NormalizeType()` 적용

`ParseBacklogHistoryMD()`:
- `Status: "resolved"` → `Status: StatusResolved`
- severity, scope, type: Normalize 적용
- resolution: `NormalizeResolution()` 적용

`ImportItems()`:
- `item.Status == "resolved"` → `item.Status == StatusResolved`

- [ ] **Step 2: export.go — Status 필터 const 사용**

`Export()`:
- `ListFilter{..., Status: "open"}` → `ListFilter{..., Status: StatusOpen}`

- [ ] **Step 3: 빌드 + 커밋**

```bash
go build ./cmd/apex-agent
git add internal/modules/backlog/import.go internal/modules/backlog/export.go
git commit -m "feat(tools): BACKLOG-126 import/export 대문자 UPPER_SNAKE_CASE 전환"
git push
```

---

## Task 4: Handoff 마이그레이션 v2 (junction 테이블)

**Files:**
- Modify: `internal/modules/handoff/module.go`

- [ ] **Step 1: module.go에 마이그레이션 v2 추가**

```go
mig.Register("handoff", 2, func(s *store.Store) error {
    // 1. junction 테이블 생성
    _, err := s.Exec(`CREATE TABLE branch_backlogs (
        branch     TEXT    NOT NULL,
        backlog_id INTEGER NOT NULL,
        PRIMARY KEY (branch, backlog_id)
    )`)
    if err != nil {
        return fmt.Errorf("create branch_backlogs: %w", err)
    }

    // 2. 기존 branches.backlog_id 데이터 이관 (NULL 스킵)
    _, err = s.Exec(`
        INSERT INTO branch_backlogs (branch, backlog_id)
        SELECT branch, backlog_id FROM branches WHERE backlog_id IS NOT NULL
    `)
    if err != nil {
        return fmt.Errorf("migrate backlog_ids: %w", err)
    }

    // 3. branches 재생성 (backlog_id 컬럼 제거)
    _, err = s.Exec(`
        CREATE TABLE branches_v2 (
            branch      TEXT    PRIMARY KEY,
            workspace   TEXT    NOT NULL,
            status      TEXT    NOT NULL,
            summary     TEXT,
            created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
            updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
        );
        INSERT INTO branches_v2 (branch, workspace, status, summary, created_at, updated_at)
            SELECT branch, workspace, status, summary, created_at, updated_at FROM branches;
        DROP TABLE branches;
        ALTER TABLE branches_v2 RENAME TO branches;
    `)
    return err
})
```

- [ ] **Step 2: 빌드 확인**

Run: `go build ./cmd/apex-agent`
Expected: 빌드 실패 — manager.go가 아직 branches.backlog_id를 참조. Task 5에서 수정.

- [ ] **Step 3: 커밋**

```bash
git add internal/modules/handoff/module.go
git commit -m "feat(tools): BACKLOG-126 handoff 마이그레이션 v2 — branch_backlogs junction 테이블"
git push
```

---

## Task 5: Handoff manager.go + gate.go 갱신

**Files:**
- Modify: `internal/modules/handoff/manager.go`
- Modify: `internal/modules/handoff/module.go` (notifyStartParams 변경)
- Modify: `internal/modules/handoff/gate.go`

**Ref:** 스펙 §3, §5, §6

- [ ] **Step 1: manager.go — Branch 구조체에서 BacklogID 제거, BacklogIDs 추가**

```go
type Branch struct {
    Branch    string
    Workspace string
    Status    string
    BacklogIDs []int  // junction 테이블에서 조회
    Summary   string
    CreatedAt string
    UpdatedAt string
}
```

- [ ] **Step 2: manager.go — NotifyStart 시그니처 변경**

```go
func (m *Manager) NotifyStart(branch, workspace, summary string, backlogIDs []int, scopes string, skipDesign bool) (int, error) {
```

- 기존 `nullableInt(backlogID)` INSERT 제거
- branches INSERT에서 backlog_id 컬럼 제거
- backlogIDs가 비어있지 않으면:
  - 각 ID에 대해 FIXING 상태 체크 (이미 FIXING이면 에러)
  - branch_backlogs에 INSERT
  - backlog status → FIXING (backlog.Manager 필요 — 생성자 주입)

Manager에 backlogManager 필드 추가:
```go
type Manager struct {
    store          *store.Store
    backlogManager BacklogStatusSetter
}

// BacklogStatusSetter is the interface handoff needs from backlog.
type BacklogStatusSetter interface {
    SetStatus(id int, status string) error
    Check(id int) (exists bool, status string, err error)
}
```

NewManager 시그니처 변경: `func NewManager(s *store.Store, bm BacklogStatusSetter) *Manager`

- [ ] **Step 3: manager.go — BacklogCheck junction 대응**

```go
func (m *Manager) BacklogCheck(backlogID int) (available bool, branch string, err error) {
    row := m.store.QueryRow(
        `SELECT bb.branch FROM branch_backlogs bb
         JOIN branches b ON b.branch = bb.branch
         WHERE bb.backlog_id = ? AND b.status != ?`,
        backlogID, StatusMergeNotified,
    )
    // ... 기존 로직 동일
}
```

- [ ] **Step 4: manager.go — GetBranch junction 대응**

```go
func (m *Manager) GetBranch(branch string) (*Branch, error) {
    row := m.store.QueryRow(
        `SELECT branch, workspace, status, summary, created_at, updated_at
         FROM branches WHERE branch = ?`, branch,
    )
    // ... scan (BacklogID 제거)

    // junction에서 backlog IDs 조회
    rows, err := m.store.Query(
        `SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch,
    )
    // ... scan into b.BacklogIDs
    return &b, nil
}
```

- [ ] **Step 5: module.go — New() 시그니처 변경, notifyStartParams 변경**

```go
func New(s *store.Store, bm BacklogStatusSetter) *Module {
    return &Module{manager: NewManager(s, bm)}
}
```

`notifyStartParams`:
```go
type notifyStartParams struct {
    Branch     string `json:"branch"`
    Workspace  string `json:"workspace"`
    Summary    string `json:"summary"`
    BacklogIDs []int  `json:"backlog_ids"`
    Scopes     string `json:"scopes"`
    SkipDesign bool   `json:"skip_design"`
}
```

`handleNotifyStart`:
```go
id, err := m.manager.NotifyStart(p.Branch, p.Workspace, p.Summary, p.BacklogIDs, p.Scopes, p.SkipDesign)
```

daemon_cmd.go에서 모듈 등록 순서 변경 (handoff가 backlog를 참조):
```go
backlogMod := backlogmod.New(d.Store())
d.Register(hook.New())
d.Register(backlogMod)
d.Register(handoffmod.New(d.Store(), backlogMod.Manager()))
d.Register(queuemod.New(d.Store()))
```

backlog module.go에 `Manager()` 접근자 추가:
```go
func (m *Module) Manager() *Manager { return m.manager }
```

- [ ] **Step 6: gate.go — ValidateMergeGate에 FIXING 백로그 차단**

```go
func (m *Manager) ValidateMergeGate(branch string) error {
    // 기존: 미ack 알림 체크
    notifications, err := m.CheckNotifications(branch)
    if err != nil {
        return err
    }
    if len(notifications) > 0 {
        return fmt.Errorf("차단: 미ack 알림 %d건. 먼저 ack 처리 후 머지 재시도", len(notifications))
    }

    // 신규: FIXING 백로그 체크
    rows, err := m.store.Query(
        `SELECT bb.backlog_id FROM branch_backlogs bb
         JOIN backlog_items bi ON bi.id = bb.backlog_id
         WHERE bb.branch = ? AND bi.status = 'FIXING'`,
        branch,
    )
    if err != nil {
        return fmt.Errorf("query fixing backlogs: %w", err)
    }
    defer rows.Close()
    var fixingIDs []int
    for rows.Next() {
        var id int
        rows.Scan(&id)
        fixingIDs = append(fixingIDs, id)
    }
    if len(fixingIDs) > 0 {
        return fmt.Errorf("차단: 미해결 백로그 %d건 (IDs: %v) — resolve 또는 release 후 재시도", len(fixingIDs), fixingIDs)
    }

    return nil
}
```

- [ ] **Step 7: 빌드 확인**

Run: `go build ./cmd/apex-agent`
Expected: 빌드 성공

- [ ] **Step 8: 커밋**

```bash
git add internal/modules/handoff/ internal/modules/backlog/module.go internal/cli/daemon_cmd.go
git commit -m "feat(tools): BACKLOG-126 핸드오프-백로그 연동 — junction + FIXING 전이 + 머지 차단"
git push
```

---

## Task 6: CLI 변경

**Files:**
- Modify: `internal/cli/handoff_cmd.go`
- Modify: `internal/cli/backlog_cmd.go`

- [ ] **Step 1: handoff_cmd.go — notify start 변경 + start job 서브커맨드**

기존 `handoffNotifyStartCmd()`:
- `--backlog int` → `--backlog []int` (IntSliceVar)
- backlog 슬라이스가 비어있으면 에러: "백로그 작업은 --backlog 필수. 비백로그 작업은 'start job' 사용"
- IPC params: `"backlog_ids": backlogs` (슬라이스)

신규 `handoffNotifyStartJobCmd()`:
```go
func handoffNotifyStartJobCmd() *cobra.Command {
    // --summary, --scopes, --skip-design (--backlog 없음)
    // IPC params: backlog_ids를 빈 슬라이스로 전송
}
```

notify 커맨드에 job 추가: `notify.AddCommand(handoffNotifyStartJobCmd())`

handoff status에서 `BacklogID float64` → 복수 ID 표시.

- [ ] **Step 2: backlog_cmd.go — release 커맨드 + 기존 수정**

```go
func backlogReleaseCmd() *cobra.Command {
    var reason string
    cmd := &cobra.Command{
        Use:   "release ID",
        Short: "백로그 항목 착수 해제 (FIXING → OPEN)",
        Args:  cobra.ExactArgs(1),
        RunE: func(cmd *cobra.Command, args []string) error {
            var id int
            fmt.Sscanf(args[0], "%d", &id)
            branch := getBranchID()
            params := map[string]any{"id": id, "reason": reason, "branch": branch}
            resp, err := sendBacklogRequest("release", params)
            // ...
        },
    }
    cmd.Flags().StringVar(&reason, "reason", "", "해제 사유 (필수)")
    _ = cmd.MarkFlagRequired("reason")
    return cmd
}
```

backlogCmd()에 `cmd.AddCommand(backlogReleaseCmd())` 추가.

resolve 도움말: `"FIXED, WONTFIX, DUPLICATE, DEFERRED"` → `"FIXED, DOCUMENTED, WONTFIX, DUPLICATE, SUPERSEDED"`

list 기본값: `status` 기본값 `"open"` → `"OPEN"`

- [ ] **Step 3: 빌드 + 커밋**

```bash
go build ./cmd/apex-agent
git add internal/cli/handoff_cmd.go internal/cli/backlog_cmd.go
git commit -m "feat(tools): BACKLOG-126 CLI — start job 분리 + backlog release + 대문자 전환"
git push
```

---

## Task 7: 테스트 갱신

**Files:**
- Modify: `internal/modules/backlog/*_test.go`
- Modify: `internal/modules/handoff/*_test.go`
- Modify: `e2e/backlog_test.go`
- Modify: `e2e/handoff_test.go`
- Modify: `e2e/testenv/env.go` (handoff.New 시그니처 변경 반영)

- [ ] **Step 1: backlog 단위 테스트 — 하드코딩 문자열 const 전환**

모든 `"open"` → `"OPEN"`, `"resolved"` → `"RESOLVED"`, `"bug"` → `"BUG"` 등.
`Severity: "CRITICAL"` 은 이미 대문자라 변경 없음.
`Type: "bug"` → `"BUG"`, `Status: "open"` → `"OPEN"` 등.

- [ ] **Step 2: handoff 단위 테스트 — junction + 시그니처 변경**

- `NotifyStart(..., backlogID, ...)` → `NotifyStart(..., []int{backlogID}, ...)`
- `BacklogCheck` 테스트: junction 테이블 기반 검증
- `Branch.BacklogID` → `Branch.BacklogIDs` 접근 변경
- `handoff.New(s)` → `handoff.New(s, backlogManager)` (mock 또는 nil)

- [ ] **Step 3: testenv/env.go — 모듈 등록 순서 변경**

```go
backlogMod := backlog.New(d.Store())
d.Register(hook.New())
d.Register(backlogMod)
d.Register(handoff.New(d.Store(), backlogMod.Manager()))
d.Register(queue.New(d.Store()))
```

- [ ] **Step 4: E2E 테스트 — 대문자 enum + 신규 시나리오**

e2e/backlog_test.go: `"MINOR"` → 이미 대문자. `"bug"` → `"BUG"` 등.
release 시나리오 추가.

e2e/handoff_test.go: `backlog_id` → `backlog_ids` 슬라이스.
job 모드 테스트 추가.

- [ ] **Step 5: 전체 테스트**

Run: `go test ./... -count=1 -timeout 120s`
Expected: 전체 PASS

- [ ] **Step 6: 커밋**

```bash
git add internal/modules/ e2e/ internal/cli/
git commit -m "test(tools): BACKLOG-126 테스트 갱신 — 대문자 enum + junction + release"
git push
```

---

## Task 8: 문서 갱신

**Files:**
- Modify: `docs/CLAUDE.md`

- [ ] **Step 1: docs/CLAUDE.md 스코프/타입 태그 대문자 갱신**

```
**스코프 태그**: `CORE | SHARED | GATEWAY | AUTH_SVC | CHAT_SVC | INFRA | CI | DOCS | TOOLS`
**타입 태그**: `BUG | DESIGN_DEBT | TEST | DOCS | PERF | SECURITY | INFRA`
```

Status 관련: `open` → `OPEN`, `resolved` → `RESOLVED`

- [ ] **Step 2: 커밋**

```bash
git add docs/CLAUDE.md
git commit -m "docs(tools): BACKLOG-126 백로그 스코프/타입/상태 태그 UPPER_SNAKE_CASE 갱신"
git push
```

---

## 완료 기준

1. `go test ./... -count=1` — 전체 PASS
2. `go build ./cmd/apex-agent` — 빌드 성공
3. `apex-agent backlog list` — 대문자 포맷으로 출력
4. `handoff notify start --backlog 126 --summary "..."` — FIXING 전이 + junction INSERT
5. `handoff notify start job --summary "..."` — 비백로그 작업 등록
6. `backlog release --id N --reason "..."` — FIXING → OPEN + 사유 기록
7. `backlog resolve --id N --resolution FIXED` — Resolution 검증
8. 머지 시점 FIXING 백로그 → 차단
9. 마이그레이션 v3 (backlog) + v2 (handoff) — 기존 데이터 정규화 + 보존
