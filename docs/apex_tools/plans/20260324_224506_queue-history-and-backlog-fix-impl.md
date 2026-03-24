# 큐 히스토리 이벤트 로그 + 백로그 updated_at 버그 수정 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 백로그 import 시 updated_at 보존 + 큐 상태 전이 이벤트 로그 + 큐 페이지 레이아웃 개선

**Architecture:** 백로그 버그는 manage.go의 UpdateFromImport에 변경 감지 추가. 큐 히스토리는 queue_history 테이블 신설 후 상태 전이 시 INSERT. 대시보드는 queue_history 기반 이벤트 뷰 + HTMX 무한 스크롤.

**Tech Stack:** Go, SQLite, HTMX, Go html/template

---

### Task 1: 백로그 UpdateFromImport 변경 감지

**Files:**
- Modify: `internal/modules/backlog/manage.go:302-321`
- Test: `internal/modules/backlog/manage_test.go` (신규 테스트 추가)

- [ ] **Step 1: 테스트 작성 — updated_at 보존 확인**

`manage_test.go`에 다음 테스트를 추가한다. 기존 테스트 패턴(`setupTestDB` + `NewManager`)을 따른다. 이 파일은 `package backlog` (internal test)이므로 `backlog.` prefix를 쓰지 않는다. **`"time"` import 추가 필수**.

```go
// TestUpdateFromImport_PreservesUpdatedAt: import with identical fields does not change updated_at.
func TestUpdateFromImport_PreservesUpdatedAt(t *testing.T) {
	s := setupTestDB(t)
	mgr := NewManager(s)

	// Add an item.
	item := &BacklogItem{
		Title:       "test item",
		Severity:    "MAJOR",
		Timeframe:   "IN_VIEW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "desc",
	}
	if err := mgr.Add(item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	// Read initial updated_at.
	got, err := mgr.Get(item.ID)
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	originalUpdatedAt := got.UpdatedAt

	// Sleep briefly to ensure time difference if updated_at changes.
	time.Sleep(1100 * time.Millisecond)

	// Import with identical fields — updated_at should NOT change.
	err = mgr.UpdateFromImport(item.ID, got.Title, got.Severity, got.Timeframe,
		got.Scope, got.Type, got.Description, got.Related, got.Position, got.Status)
	if err != nil {
		t.Fatalf("UpdateFromImport: %v", err)
	}

	got2, err := mgr.Get(item.ID)
	if err != nil {
		t.Fatalf("Get after import: %v", err)
	}
	if got2.UpdatedAt != originalUpdatedAt {
		t.Errorf("updated_at changed for identical import: %q → %q", originalUpdatedAt, got2.UpdatedAt)
	}
}

// TestUpdateFromImport_UpdatesOnChange: import with changed fields updates updated_at.
func TestUpdateFromImport_UpdatesOnChange(t *testing.T) {
	s := setupTestDB(t)
	mgr := NewManager(s)

	item := &BacklogItem{
		Title:       "test item",
		Severity:    "MAJOR",
		Timeframe:   "IN_VIEW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "desc",
	}
	if err := mgr.Add(item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	got, err := mgr.Get(item.ID)
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	originalUpdatedAt := got.UpdatedAt

	time.Sleep(1100 * time.Millisecond)

	// Import with changed title — updated_at SHOULD change.
	err = mgr.UpdateFromImport(item.ID, "changed title", got.Severity, got.Timeframe,
		got.Scope, got.Type, got.Description, got.Related, got.Position, got.Status)
	if err != nil {
		t.Fatalf("UpdateFromImport: %v", err)
	}

	got2, err := mgr.Get(item.ID)
	if err != nil {
		t.Fatalf("Get after import: %v", err)
	}
	if got2.UpdatedAt == originalUpdatedAt {
		t.Error("expected updated_at to change for modified import")
	}
	if got2.Title != "changed title" {
		t.Errorf("expected title='changed title', got %q", got2.Title)
	}
}
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Run: `go test ./internal/modules/backlog/... -run TestUpdateFromImport_ -v -count=1`
Expected: `TestUpdateFromImport_PreservesUpdatedAt` FAIL (updated_at이 변경됨)

- [ ] **Step 3: UpdateFromImport 구현 수정**

`manage.go`의 `UpdateFromImport`를 다음으로 교체:

```go
// UpdateFromImport updates metadata fields for an existing item from MD import.
// Only updates (and bumps updated_at) if at least one field actually changed.
// Does NOT touch resolution/resolved_at — those are managed by Resolve().
func (m *Manager) UpdateFromImport(id int, title, severity, timeframe, scope, itemType, description, related string, position int, status string) error {
	// Read current values to detect changes.
	existing, err := m.Get(id)
	if err != nil {
		return fmt.Errorf("UpdateFromImport #%d: read existing: %w", id, err)
	}
	if existing == nil {
		return fmt.Errorf("UpdateFromImport #%d: item not found", id)
	}

	// Compare all import-managed fields.
	if existing.Title == title &&
		existing.Severity == severity &&
		existing.Timeframe == timeframe &&
		existing.Scope == scope &&
		existing.Type == itemType &&
		existing.Description == description &&
		existing.Related == related &&
		existing.Position == position &&
		existing.Status == status {
		// No changes — skip update entirely.
		return nil
	}

	_, err = m.q.Exec(`
		UPDATE backlog_items
		SET title = ?, severity = ?, timeframe = ?, scope = ?, type = ?,
		    description = ?, related = ?, position = ?, status = ?,
		    updated_at = datetime('now','localtime')
		WHERE id = ?`,
		title, severity, timeframe, scope, itemType,
		description, related, position, status, id,
	)
	if err != nil {
		return fmt.Errorf("UpdateFromImport #%d: %w", id, err)
	}
	ml.Info("item updated from import", "id", id, "severity", severity, "timeframe", timeframe)
	return nil
}
```

- [ ] **Step 4: 테스트 실행 — 성공 확인**

Run: `go test ./internal/modules/backlog/... -run TestUpdateFromImport_ -v -count=1`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add internal/modules/backlog/manage.go internal/modules/backlog/manage_test.go
git commit -m "fix(backlog): import 시 변경 없는 항목의 updated_at 보존

UpdateFromImport에 변경 감지 로직 추가 — 모든 필드가 동일하면
UPDATE를 스킵하여 원본 타임스탬프 보존."
```

---

### Task 2: queue_history 테이블 마이그레이션 + insertHistory 헬퍼

**Files:**
- Modify: `internal/modules/queue/module.go:52-56`
- Modify: `internal/modules/queue/manager.go`
- Test: `internal/modules/queue/manager_test.go`

- [ ] **Step 1: 테스트 작성 — DashboardHistory 조회**

`manager_test.go`에 추가:

```go
// TestDashboardHistory_RecordsEvents: TryAcquire+Release writes history events.
func TestDashboardHistory_RecordsEvents(t *testing.T) {
	m := newTestManager(t)

	// Free channel → ACTIVE directly (no WAITING).
	_, err := m.TryAcquire(context.Background(), "build", "feature/foo", os.Getpid())
	if err != nil {
		t.Fatalf("TryAcquire: %v", err)
	}

	// Release → DONE.
	if err := m.Release("build"); err != nil {
		t.Fatalf("Release: %v", err)
	}

	entries, err := m.DashboardHistory("build", 0, 50, "", "")
	if err != nil {
		t.Fatalf("DashboardHistory: %v", err)
	}

	// Expect 2 events: ACTIVE, DONE (newest first).
	if len(entries) != 2 {
		t.Fatalf("expected 2 history entries, got %d", len(entries))
	}
	if entries[0].Status != queue.StatusDone {
		t.Errorf("expected first entry status=DONE, got %q", entries[0].Status)
	}
	if entries[1].Status != queue.StatusActive {
		t.Errorf("expected second entry status=ACTIVE, got %q", entries[1].Status)
	}
}

// TestDashboardHistory_WaitingEvent: busy channel records WAITING then ACTIVE.
func TestDashboardHistory_WaitingEvent(t *testing.T) {
	s := newTestStore(t)
	m := queue.NewManager(s)
	pid := os.Getpid()

	// Hold the lock.
	_, _ = m.TryAcquire(context.Background(), "build", "feature/holder", pid)

	// Second branch → WAITING.
	_, _ = m.TryAcquire(context.Background(), "build", "feature/waiter", pid)

	entries, err := m.DashboardHistory("build", 0, 50, "", "")
	if err != nil {
		t.Fatalf("DashboardHistory: %v", err)
	}

	// Expect: WAITING(waiter), ACTIVE(holder) — newest first.
	if len(entries) < 2 {
		t.Fatalf("expected at least 2 history entries, got %d", len(entries))
	}
	if entries[0].Status != queue.StatusWaiting || entries[0].Branch != "feature/waiter" {
		t.Errorf("expected WAITING for feature/waiter, got %s for %s", entries[0].Status, entries[0].Branch)
	}
}

// TestDashboardHistory_Pagination: offset+limit work correctly.
func TestDashboardHistory_Pagination(t *testing.T) {
	m := newTestManager(t)
	pid := os.Getpid()

	// Create 3 events: ACTIVE, DONE, ACTIVE.
	_, _ = m.TryAcquire(context.Background(), "build", "feature/a", pid)
	_ = m.Release("build")
	_, _ = m.TryAcquire(context.Background(), "build", "feature/b", pid)

	// Limit 2 → should get 2 newest.
	entries, err := m.DashboardHistory("build", 0, 2, "", "")
	if err != nil {
		t.Fatalf("DashboardHistory limit: %v", err)
	}
	if len(entries) != 2 {
		t.Errorf("expected 2 entries with limit=2, got %d", len(entries))
	}

	// Offset 2 → should get 1 oldest.
	entries2, err := m.DashboardHistory("build", 2, 2, "", "")
	if err != nil {
		t.Fatalf("DashboardHistory offset: %v", err)
	}
	if len(entries2) != 1 {
		t.Errorf("expected 1 entry with offset=2, got %d", len(entries2))
	}
}
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Run: `go test ./internal/modules/queue/... -run TestDashboardHistory -v -count=1`
Expected: 컴파일 에러 (DashboardHistory 미존재)

- [ ] **Step 3: 마이그레이션 v4 추가**

`module.go`에 `RegisterSchema`의 v3 다음에 추가:

```go
mig.Register("queue", 4, func(tx *store.TxStore) error {
	_, err := tx.Exec(`CREATE TABLE queue_history (
		id         INTEGER PRIMARY KEY AUTOINCREMENT,
		channel    TEXT NOT NULL,
		branch     TEXT NOT NULL,
		status     TEXT NOT NULL,
		timestamp  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
	)`)
	if err != nil {
		return err
	}
	_, err = tx.Exec(`CREATE INDEX idx_queue_history_channel_ts ON queue_history(channel, timestamp DESC)`)
	return err
})
```

- [ ] **Step 4: insertHistory 헬퍼 + DashboardHistory 조회 구현**

`manager.go`에 추가:

```go
// HistoryEntry represents a row in the queue_history table.
type HistoryEntry struct {
	ID        int
	Channel   string
	Branch    string
	Status    string
	Timestamp string
}

// insertHistory records a state-transition event in queue_history.
func (m *Manager) insertHistory(channel, branch, status string) {
	_, err := m.store.Exec(
		`INSERT INTO queue_history (channel, branch, status) VALUES (?, ?, ?)`,
		channel, branch, status,
	)
	if err != nil {
		ml.Warn("failed to insert queue history", "channel", channel, "branch", branch, "status", status, "err", err)
	}
}

// insertHistoryTx records a state-transition event using a transaction store.
func (m *Manager) insertHistoryTx(s store.Querier, channel, branch, status string) {
	_, err := s.Exec(
		`INSERT INTO queue_history (channel, branch, status) VALUES (?, ?, ?)`,
		channel, branch, status,
	)
	if err != nil {
		ml.Warn("failed to insert queue history (tx)", "channel", channel, "branch", branch, "status", status, "err", err)
	}
}

// DashboardHistory returns history events for a channel, newest first.
// Supports pagination (offset+limit) and optional time range filter (from/to as ISO datetime).
func (m *Manager) DashboardHistory(channel string, offset, limit int, from, to string) ([]HistoryEntry, error) {
	query := `SELECT id, channel, branch, status, timestamp FROM queue_history WHERE channel = ?`
	args := []any{channel}

	if from != "" {
		query += ` AND timestamp >= ?`
		args = append(args, from)
	}
	if to != "" {
		query += ` AND timestamp <= ?`
		args = append(args, to)
	}

	query += ` ORDER BY id DESC LIMIT ? OFFSET ?`
	args = append(args, limit, offset)

	rows, err := m.store.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("DashboardHistory: %w", err)
	}
	defer rows.Close()

	var entries []HistoryEntry
	for rows.Next() {
		var e HistoryEntry
		if err := rows.Scan(&e.ID, &e.Channel, &e.Branch, &e.Status, &e.Timestamp); err != nil {
			return nil, fmt.Errorf("DashboardHistory scan: %w", err)
		}
		entries = append(entries, e)
	}
	return entries, rows.Err()
}
```

- [ ] **Step 5: 상태 전이 지점에 insertHistory 호출 추가**

`manager.go`의 TryAcquire 내부 (트랜잭션 안에서):

1. **WAITING INSERT** (line 68): `m.insertEntryTx(...)` 뒤에 `m.insertHistoryTx(tx, channel, branch, StatusWaiting)` 추가
2. **WAITING INSERT** (line 86): 동일하게 추가
3. **WAITING→ACTIVE promote** (line 93): `tx.Exec(UPDATE...)` 성공 후 `m.insertHistoryTx(tx, channel, first.Branch, StatusActive)` 추가
4. **직접 ACTIVE** (line 102): `m.insertEntryTx(...)` 뒤에 `m.insertHistoryTx(tx, channel, branch, StatusActive)` 추가

`manager.go`의 Acquire 내부:
5. **WAITING INSERT** (line 138): `m.insertEntryTx(...)` 뒤에 `m.insertHistoryTx(tx, channel, branch, StatusWaiting)` 추가

`manager.go`의 tryPromote 내부:
6. **CAS promote** (line 211-218): `promoted = n > 0` 후 `if promoted { m.insertHistoryTx(tx, channel, branch, StatusActive) }` 추가

`manager.go`의 Release:
7. **ACTIVE→DONE** (line 247): Release에 branch 정보가 없으므로 SELECT+UPDATE+history INSERT를 **트랜잭션으로 감싸서** TOCTOU race 방지. Release 수정:

```go
func (m *Manager) Release(channel string) error {
	var released bool
	var branch string
	err := m.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
		// Read the active branch (for history).
		_ = tx.QueryRow(
			`SELECT branch FROM queue WHERE channel=? AND status=?`,
			channel, StatusActive,
		).Scan(&branch)

		res, err := tx.Exec(
			`UPDATE queue SET status=?, finished_at=datetime('now','localtime') WHERE channel=? AND status=?`,
			StatusDone, channel, StatusActive,
		)
		if err != nil {
			return fmt.Errorf("queue.Release: %w", err)
		}
		n, _ := res.RowsAffected()
		if n == 0 {
			ml.Warn("release: no active entry found (already released or stale-cleaned)", "channel", channel)
		} else {
			released = true
			m.insertHistoryTx(tx, channel, branch, StatusDone)
		}
		return nil
	})
	if err != nil {
		return err
	}
	if released {
		ml.Audit("lock released", "channel", channel)
	}
	return nil
}
```

**주의**: `Release()`에 `"context"` import가 필요할 수 있음 (이미 파일 상단에 있음).

- [ ] **Step 6: 테스트 실행 — 성공 확인**

Run: `go test ./internal/modules/queue/... -v -count=1`
Expected: 모든 테스트 PASS (기존 + 신규)

- [ ] **Step 7: 커밋**

```bash
git add internal/modules/queue/
git commit -m "feat(queue): queue_history 이벤트 로그 테이블 + 상태 전이 기록

- 마이그레이션 v4: queue_history 테이블 + 인덱스
- TryAcquire/Acquire/tryPromote/Release에서 상태 전이 시 이벤트 INSERT
- DashboardHistory(channel, offset, limit, from, to) 조회 함수"
```

---

### Task 3: HTTP 레이어 — QueueQuerier 확장 + 핸들러 + 어댑터

**Files:**
- Modify: `internal/httpd/server.go:42-46` — QueueQuerier 인터페이스
- Modify: `internal/httpd/queries.go` — HistoryEntry 타입 + queryQueueHistory 함수
- Modify: `internal/httpd/handler_queue.go` — history partial 핸들러
- Modify: `internal/httpd/routes.go` — 라우트 등록
- Modify: `internal/cli/httpd_adapters.go` — 어댑터

- [ ] **Step 1: QueueQuerier 인터페이스 확장**

`server.go`의 QueueQuerier:

```go
type QueueQuerier interface {
	DashboardQueueAll() ([]QueueEntry, error)
	DashboardLockStatus(channel string) (bool, error)
	DashboardQueueHistory(channel string, offset, limit int, from, to string) ([]QueueHistoryEntry, error)
}
```

- [ ] **Step 2: QueueHistoryEntry 타입 + queryQueueHistory 추가**

`queries.go`에 추가:

```go
type QueueHistoryEntry struct {
	ID        int
	Channel   string
	Branch    string
	Status    string
	Timestamp string
}

func queryQueueHistory(qm QueueQuerier, channel string, offset, limit int, from, to string) ([]QueueHistoryEntry, error) {
	if qm == nil {
		return nil, nil
	}
	return qm.DashboardQueueHistory(channel, offset, limit, from, to)
}
```

- [ ] **Step 3: 어댑터 구현**

`httpd_adapters.go`의 `queueQuerierAdapter`에 추가:

```go
func (a *queueQuerierAdapter) DashboardQueueHistory(channel string, offset, limit int, from, to string) ([]httpd.QueueHistoryEntry, error) {
	entries, err := a.mgr.DashboardHistory(channel, offset, limit, from, to)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.QueueHistoryEntry, len(entries))
	for i, e := range entries {
		result[i] = httpd.QueueHistoryEntry{
			ID:        e.ID,
			Channel:   e.Channel,
			Branch:    e.Branch,
			Status:    e.Status,
			Timestamp: e.Timestamp,
		}
	}
	return result, nil
}
```

- [ ] **Step 4: history partial 핸들러**

`handler_queue.go`에 추가:

```go
func (s *Server) handlePartialQueueHistory(w http.ResponseWriter, r *http.Request) {
	if s.queueMgr == nil {
		s.renderHTMXError(w, "queue not available")
		return
	}

	channel := r.URL.Query().Get("channel")
	if channel == "" {
		channel = "build"
	}

	offsetStr := r.URL.Query().Get("offset")
	offset := 0
	if offsetStr != "" {
		fmt.Sscanf(offsetStr, "%d", &offset)
	}

	limitStr := r.URL.Query().Get("limit")
	limit := 50
	if limitStr != "" {
		fmt.Sscanf(limitStr, "%d", &limit)
	}

	from := r.URL.Query().Get("from")
	to := r.URL.Query().Get("to")

	entries, err := queryQueueHistory(s.queueMgr, channel, offset, limit, from, to)
	if err != nil {
		s.renderHTMXError(w, err.Error())
		return
	}

	data := map[string]any{
		"Entries": entries,
		"Channel": channel,
		"Offset":  offset,
		"Limit":   limit,
		"From":    from,
		"To":      to,
		"HasMore": len(entries) == limit,
	}
	s.renderPartial(w, "partial-queue-history", data)
}
```

- [ ] **Step 5: 라우트 등록**

`routes.go`에 추가 (Queue partial 섹션):

```go
mux.HandleFunc("GET /partials/queue-history", s.handlePartialQueueHistory)
```

- [ ] **Step 6: `fmt` import 추가**

`handler_queue.go`의 import에 `"fmt"` 추가 (Sscanf 사용).

- [ ] **Step 7: 컴파일 확인**

Run: `go build ./...`
Expected: 성공

- [ ] **Step 7: 커밋**

```bash
git add internal/httpd/ internal/cli/httpd_adapters.go
git commit -m "feat(httpd): 큐 히스토리 API 엔드포인트 추가

- QueueQuerier에 DashboardQueueHistory 메서드 추가
- /partials/queue-history 핸들러 (channel, offset, limit, from, to 파라미터)
- httpd_adapters에 queue history 어댑터 구현"
```

---

### Task 4: 큐 페이지 템플릿 — 좌우 분리 + 필터 + 무한 스크롤

**Files:**
- Modify: `internal/httpd/templates/queue.html`
- Create: `internal/httpd/templates/partials/queue_history.html`
- Modify: `internal/httpd/templates/partials/queue_page.html` (기존 코드 교체)
- Modify: `internal/httpd/templates/layout.html` (무한 스크롤 JS 추가)

- [ ] **Step 1: queue.html 교체 — 필터 바 + 좌우 2열 레이아웃**

```html
{{define "content"}}
<h1 class="page-title">Queue</h1>

<div class="card" style="margin-bottom:1rem">
  <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap;padding:4px 0">
    <span style="font-weight:600;color:var(--text-muted);font-size:0.85rem">Filter:</span>
    <button class="rate-btn active" onclick="setQueueFilter('default')">Latest</button>
    <button class="rate-btn" onclick="setQueueFilter('today')">Today</button>
    <button class="rate-btn" onclick="setQueueFilter('24h')">24h</button>
    <button class="rate-btn" onclick="setQueueFilter('7d')">7d</button>
    <span style="color:var(--text-muted)">|</span>
    <label style="font-size:0.82rem;color:var(--text-muted)">From</label>
    <input type="datetime-local" step="1" id="queue-from" class="dt-input"
           onchange="applyCustomFilter()">
    <label style="font-size:0.82rem;color:var(--text-muted)">To</label>
    <input type="datetime-local" step="1" id="queue-to" class="dt-input"
           onchange="applyCustomFilter()">
    <button class="rate-btn" onclick="clearCustomFilter()">Clear</button>
  </div>
</div>

<div class="grid-2">
  <div class="card">
    <div class="card-header">Build Channel</div>
    <div id="build-history"
         hx-get="/partials/queue-history?channel=build&limit=50"
         hx-trigger="load"
         hx-swap="innerHTML">
    </div>
  </div>
  <div class="card">
    <div class="card-header blue">Merge Channel</div>
    <div id="merge-history"
         hx-get="/partials/queue-history?channel=merge&limit=50"
         hx-trigger="load"
         hx-swap="innerHTML">
    </div>
  </div>
</div>

<script>
var queueFilter = {from:'', to:'', preset:'default'};

function setQueueFilter(preset) {
  var now = new Date();
  var from = '';
  if (preset === 'today') {
    from = now.toISOString().slice(0,10) + 'T00:00:00';
  } else if (preset === '24h') {
    from = new Date(now - 86400000).toISOString().slice(0,19);
  } else if (preset === '7d') {
    from = new Date(now - 7*86400000).toISOString().slice(0,19);
  }
  queueFilter = {from: from, to: '', preset: preset};
  document.getElementById('queue-from').value = from ? from : '';
  document.getElementById('queue-to').value = '';
  document.querySelectorAll('.card .rate-btn').forEach(function(b){ b.classList.remove('active'); });
  event.target.classList.add('active');
  reloadHistory();
}

function applyCustomFilter() {
  queueFilter.from = document.getElementById('queue-from').value;
  queueFilter.to = document.getElementById('queue-to').value;
  queueFilter.preset = 'custom';
  document.querySelectorAll('.card .rate-btn').forEach(function(b){ b.classList.remove('active'); });
  reloadHistory();
}

function clearCustomFilter() {
  document.getElementById('queue-from').value = '';
  document.getElementById('queue-to').value = '';
  setQueueFilter('default');
}

function buildHistoryUrl(channel, offset) {
  var url = '/partials/queue-history?channel=' + channel + '&limit=50&offset=' + offset;
  if (queueFilter.from) url += '&from=' + encodeURIComponent(queueFilter.from);
  if (queueFilter.to) url += '&to=' + encodeURIComponent(queueFilter.to);
  return url;
}

function reloadHistory() {
  ['build','merge'].forEach(function(ch) {
    var el = document.getElementById(ch + '-history');
    el.innerHTML = '';
    htmx.ajax('GET', buildHistoryUrl(ch, 0), {target: el, swap: 'innerHTML'});
  });
}

// Polling: check for new events and prepend them at top.
// Uses a hidden sentinel div to fetch only events newer than the latest known ID.
var lastIds = {build: 0, merge: 0};
function pollNewEvents() {
  ['build','merge'].forEach(function(ch) {
    var el = document.getElementById(ch + '-history');
    var firstEntry = el.querySelector('[data-history-id]');
    var currentLatest = firstEntry ? parseInt(firstEntry.dataset.historyId) || 0 : 0;

    if (currentLatest > lastIds[ch]) {
      lastIds[ch] = currentLatest;
      return; // First run — just record the latest ID.
    }

    // Fetch latest 10 entries and prepend any that are newer.
    fetch('/partials/queue-history?channel=' + ch + '&limit=10&offset=0')
      .then(function(r) { return r.text(); })
      .then(function(html) {
        var tmp = document.createElement('div');
        tmp.innerHTML = html;
        var newEntries = tmp.querySelectorAll('[data-history-id]');
        var insertBefore = el.firstChild;
        for (var i = newEntries.length - 1; i >= 0; i--) {
          var newId = parseInt(newEntries[i].dataset.historyId) || 0;
          if (newId > lastIds[ch]) {
            el.insertBefore(newEntries[i], insertBefore);
            insertBefore = newEntries[i];
          }
        }
        if (newEntries.length > 0) {
          var maxId = parseInt(newEntries[0].dataset.historyId) || 0;
          if (maxId > lastIds[ch]) lastIds[ch] = maxId;
        }
      });
  });
}

// Start polling after initial load.
document.body.addEventListener('htmx:afterSwap', function(e) {
  if (e.detail.target.id === 'build-history' || e.detail.target.id === 'merge-history') {
    var first = e.detail.target.querySelector('[data-history-id]');
    if (first) {
      var ch = e.detail.target.id.replace('-history', '');
      lastIds[ch] = parseInt(first.dataset.historyId) || 0;
    }
  }
});
var pollRate = parseInt(localStorage.getItem('apexPollRate')) || 1000;
setInterval(pollNewEvents, pollRate);
</script>
{{end}}
```

- [ ] **Step 2: queue_history.html partial 생성**

`templates/partials/queue_history.html`:

```html
{{define "partial-queue-history"}}
{{if .Entries}}
{{range .Entries}}
<div class="lock-card" data-history-id="{{.ID}}">
  <div class="lock-indicator {{if eq .Status "ACTIVE"}}locked{{else if eq .Status "WAITING"}}waiting{{else}}done{{end}}"></div>
  <div style="flex:1">
    <div style="display:flex;align-items:center;gap:8px">
      <span style="font-weight:600">{{.Branch}}</span>
      <span class="badge {{if eq .Status "ACTIVE"}}badge-red{{else if eq .Status "WAITING"}}badge-amber{{else}}badge-blue{{end}}">{{.Status}}</span>
    </div>
    <div class="tl-meta">{{.Timestamp}}</div>
  </div>
</div>
{{end}}
{{if .HasMore}}
<div hx-get="{{historyUrl .Channel (add .Offset .Limit) .Limit .From .To}}"
     hx-trigger="revealed"
     hx-swap="outerHTML">
  <div class="tl-meta" style="text-align:center;padding:8px">Loading more...</div>
</div>
{{end}}
{{else}}
<div class="empty-state">No history events</div>
{{end}}
{{end}}
```

- [ ] **Step 3: 템플릿 함수 등록 — historyUrl, add**

`render.go`의 `loadAllPages()`에서 `template.New("")` 대신 함수 맵 포함:

```go
funcMap := template.FuncMap{
	"add": func(a, b int) int { return a + b },
	"historyUrl": func(channel string, offset, limit int, from, to string) string {
		url := fmt.Sprintf("/partials/queue-history?channel=%s&offset=%d&limit=%d", channel, offset, limit)
		if from != "" {
			url += "&from=" + from
		}
		if to != "" {
			url += "&to=" + to
		}
		return url
	},
}
partials, err := template.New("").Funcs(funcMap).ParseFS(templateFS, "templates/partials/*.html")
```

또한 `renderPartial`에서 `_partials` 대신 페이지별 템플릿셋을 사용해야 funcMap이 적용되므로, `loadAllPages`의 `partials` 생성 시 funcMap을 적용한다.

- [ ] **Step 4: queue_page.html 교체 — 대시보드 메인용 컴팩트 유지**

기존 `queue_page.html`은 queue 전용 페이지의 풀 사이즈 뷰였으나, 이제 queue.html이 직접 히스토리를 로드하므로 이 파일은 더 이상 사용되지 않는다. queue.html의 `hx-get="/partials/queue-page"` 제거로 인해 불필요.

handler_queue.go의 `handleQueue`에서 Entries를 넘기던 로직 제거 — queue.html이 자체적으로 HTMX로 로드.

```go
func (s *Server) handleQueue(w http.ResponseWriter, r *http.Request) {
	s.renderPage(w, "queue", map[string]any{"Page": "queue"})
}
```

`handlePartialQueuePage`와 라우트 제거:
- `handler_queue.go`에서 `handlePartialQueuePage` 함수 삭제
- `routes.go`에서 `mux.HandleFunc("GET /partials/queue-page", s.handlePartialQueuePage)` (line 24) 삭제
- `queue_page.html`은 삭제하거나 빈 파일로 둠 (embed.FS가 *.html 글로빙하므로 삭제 시 정합성 확인)

- [ ] **Step 5: CSS 추가 — datetime input 스타일**

`internal/httpd/static/style.css`에 추가:

```css
.dt-input {
  background: var(--card-bg);
  border: 1px solid var(--border);
  color: var(--text);
  padding: 4px 8px;
  border-radius: 6px;
  font-size: 0.82rem;
  font-family: inherit;
}
.dt-input::-webkit-calendar-picker-indicator {
  filter: invert(0.8);
}
```

- [ ] **Step 6: 컴파일 + 수동 확인**

Run: `go build ./...`
Expected: 성공

데몬 재시작 후 `localhost:7600/queue`에서:
- 필터 버튼 동작
- 좌우 2열 레이아웃
- 빈 히스토리 시 "No history events" 표시

- [ ] **Step 7: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(httpd): 큐 페이지 좌우 분리 + 히스토리 이벤트 목록 + 시간 범위 필터

- Build/Merge 채널 좌우 grid-2 레이아웃
- 프리셋 필터 (Today/24h/7d) + 커스텀 datetime-local picker
- HTMX revealed 트리거 무한 스크롤 (50개 단위)
- 대시보드 메인 queue_status는 기존 ACTIVE/WAITING 요약 유지"
```

---

### Task 5: 전체 테스트 + 빌드 검증

**Files:** (수정 없음 — 검증만)

- [ ] **Step 1: Go 전체 테스트**

Run: `go test ./... -count=1`
Expected: PASS

- [ ] **Step 2: Go 빌드**

Run: `go build -o apex-agent.exe ./cmd/apex-agent`
Expected: 성공

- [ ] **Step 3: 데몬 재시작 + 대시보드 확인**

```bash
apex-agent daemon stop
cp apex-agent.exe "$LOCALAPPDATA/apex-agent/apex-agent.exe"
apex-agent daemon start
```

브라우저에서 `localhost:7600/queue` 확인:
- 필터 버튼 3종 동작
- datetime picker 초 단위 동작
- 무한 스크롤 동작 (이벤트가 50개 이상이면)
- 대시보드 메인 큐 위젯 정상 (ACTIVE/WAITING 요약)

- [ ] **Step 4: backlog export 테스트**

```bash
apex-agent backlog export --stdout | head -5
```
Expected: updated_at 값이 항목별로 다른 타임스탬프 (모두 같은 시간이 아님)
