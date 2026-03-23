# 백로그 정합성 강화 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 백로그 DB↔MD 동기화 엣지 케이스를 근본 해결하고, 모든 백로그 조작을 CLI 경유로 강제한다.

**Architecture:** backlog 패키지에 ExportHistory/Update 추가, export CLI를 파일 직접 쓰기로 변경, validate-backlog hook 신설, MergePipeline 순서 재배치 (commit→checkout→IPC).

**Tech Stack:** Go 1.23, cobra CLI, SQLite, Claude Code PreToolUse hook

**Spec:** `docs/apex_tools/plans/20260323_183955_backlog_integrity_spec.md`

---

## 파일 구조

| 파일 | 역할 | 변경 |
|------|------|------|
| `internal/modules/backlog/export.go` | ExportHistory 함수 추가 | **수정** |
| `internal/modules/backlog/manage.go` | Update 메서드 추가 | **수정** |
| `internal/modules/backlog/module.go` | update 라우트 등록 | **수정** |
| `internal/modules/backlog/export_test.go` | ExportHistory 단위 테스트 | **신규** |
| `internal/modules/backlog/manage_test.go` | Update 단위 테스트 | **수정 또는 신규** |
| `internal/cli/backlog_cmd.go` | update CLI + export 수정 | **수정** |
| `internal/workflow/sync.go` | SyncExport에 HISTORY 쓰기 추가 | **수정** |
| `internal/workflow/sync_test.go` | HISTORY round-trip 테스트 | **수정** |
| `internal/workflow/pipeline.go` | MergePipeline 순서 재배치 | **수정** |
| `internal/workflow/pipeline_test.go` | MergePipeline 순서 검증 | **수정** |
| `internal/modules/hook/gate.go` | ValidateBacklog 함수 추가 | **수정** |
| `internal/modules/hook/gate_test.go` | ValidateBacklog 단위 테스트 | **수정** |
| `internal/cli/hook_cmd.go` | validate-backlog 서브커맨드 | **수정** |
| `.claude/settings.json` | validate-backlog hook 등록 | **수정** |
| `apex_tools/apex-agent/CLAUDE.md` | SoT 보정 + 가이드 | **수정** |
| `CLAUDE.md` | 백로그 직접 편집 금지 규칙 | **수정** |

---

### Task 1: ExportHistory — RESOLVED 항목 HISTORY 이관

**Files:**
- Modify: `internal/modules/backlog/export.go`
- Create: `internal/modules/backlog/export_test.go`

- [ ] **Step 1: ExportHistory 테스트 작성**

`internal/modules/backlog/export_test.go`:
```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupExportTest(t *testing.T) (*Manager, func()) {
	t.Helper()
	s, err := store.Open(t.TempDir() + "/test.db")
	if err != nil {
		t.Fatal(err)
	}
	mig := store.NewMigrator(s)
	mod := New(s)
	mod.RegisterSchema(mig)
	mig.Migrate()
	return mod.Manager(), func() { s.Close() }
}

func TestExportHistory_NewResolved(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	// 항목 추가 후 resolve
	mgr.Add(&BacklogItem{
		ID: 1, Title: "해결된 이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "테스트 설명",
	})
	mgr.Resolve(1, "FIXED")

	existingHistory := "# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n\n### #99. 기존 항목\n- old\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	if !strings.Contains(result, "해결된 이슈") {
		t.Error("should contain resolved item title")
	}
	if !strings.Contains(result, "#99. 기존 항목") {
		t.Error("should preserve existing history")
	}
	// 마커 아래에 삽입되었는지
	markerIdx := strings.Index(result, "<!-- NEW_ENTRY_BELOW -->")
	itemIdx := strings.Index(result, "해결된 이슈")
	if itemIdx < markerIdx {
		t.Error("resolved item should be after marker")
	}
}

func TestExportHistory_NoDuplicates(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "이미 있는 이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(1, "FIXED")

	// 기존 HISTORY에 #1이 이미 있음
	existingHistory := "# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n\n### #1. 이미 있는 이슈\n- old\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	// #1이 중복 삽입되지 않아야 함
	count := strings.Count(result, "### #1.")
	if count != 1 {
		t.Errorf("expected 1 occurrence of #1, got %d", count)
	}
}

func TestExportHistory_NoMarker(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(1, "FIXED")

	// 마커 없는 HISTORY
	existingHistory := "# BACKLOG HISTORY\n\n### #99. 기존\n- old\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	// 마커가 복원되었는지
	if !strings.Contains(result, "<!-- NEW_ENTRY_BELOW -->") {
		t.Error("should restore marker")
	}
	if !strings.Contains(result, "이슈") {
		t.Error("should contain new item")
	}
}

func TestExportHistory_NoResolved(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	// OPEN 항목만 — HISTORY 변경 없어야 함
	mgr.Add(&BacklogItem{
		ID: 1, Title: "열린 이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	existingHistory := "# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	if result != existingHistory {
		t.Errorf("should not modify history when no resolved items\ngot: %q", result)
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/modules/backlog/... -run TestExportHistory -v`
Expected: FAIL — `ExportHistory` 미정의

- [ ] **Step 3: ExportHistory 구현**

`export.go`에 추가:
```go
// ExportHistory generates updated BACKLOG_HISTORY.md content.
// New RESOLVED items (not already in existingHistory) are prepended
// after the <!-- NEW_ENTRY_BELOW --> marker.
func (mgr *Manager) ExportHistory(existingHistory string) (string, error) {
	// 1. DB에서 RESOLVED 항목 조회
	resolved, err := mgr.List(ListFilter{Status: StatusResolved})
	if err != nil {
		return existingHistory, fmt.Errorf("list resolved: %w", err)
	}
	if len(resolved) == 0 {
		return existingHistory, nil
	}

	// 2. 기존 HISTORY에서 이미 있는 ID 추출
	existingIDs := parseHistoryIDs(existingHistory)

	// 3. 신규 RESOLVED만 필터
	var newItems []BacklogItem
	for _, item := range resolved {
		if !existingIDs[item.ID] {
			newItems = append(newItems, item)
		}
	}
	if len(newItems) == 0 {
		return existingHistory, nil
	}

	// 4. 히스토리 항목 포맷
	var insertion strings.Builder
	for _, item := range newItems {
		writeHistoryItem(&insertion, &item)
	}

	// 5. 마커 위치에 삽입
	const marker = "<!-- NEW_ENTRY_BELOW -->"
	idx := strings.Index(existingHistory, marker)
	if idx < 0 {
		// 마커 없으면 헤더 직후에 복원
		header := "# BACKLOG HISTORY\n\n"
		if strings.HasPrefix(existingHistory, header) {
			return header + marker + "\n\n" + insertion.String() + existingHistory[len(header):], nil
		}
		return existingHistory + "\n" + marker + "\n\n" + insertion.String(), nil
	}

	insertPos := idx + len(marker) + 1 // marker 다음 줄
	return existingHistory[:insertPos] + "\n" + insertion.String() + existingHistory[insertPos:], nil
}

// parseHistoryIDs extracts item IDs from existing HISTORY content.
func parseHistoryIDs(content string) map[int]bool {
	ids := make(map[int]bool)
	for _, m := range itemHeaderRe.FindAllStringSubmatch(content, -1) {
		id, _ := strconv.Atoi(m[1])
		ids[id] = true
	}
	return ids
}

// writeHistoryItem formats a single item in BACKLOG_HISTORY.md format.
func writeHistoryItem(b *strings.Builder, item *BacklogItem) {
	fmt.Fprintf(b, "### #%d. %s\n", item.ID, item.Title)
	fmt.Fprintf(b, "- **등급**: %s | **스코프**: %s | **타입**: %s\n",
		item.Severity, item.Scope, item.Type)
	resolvedAt := item.ResolvedAt
	if resolvedAt == "" {
		resolvedAt = "—"
	}
	resolution := item.Resolution
	if resolution == "" {
		resolution = "—"
	}
	fmt.Fprintf(b, "- **해결**: %s | **방식**: %s\n", resolvedAt, resolution)
	desc := item.Description
	if desc == "" {
		desc = "—"
	}
	fmt.Fprintf(b, "- **비고**: %s\n\n", desc)
}
```

`export.go` import에 `"strconv"` 추가 필요.

- [ ] **Step 4: 테스트 통과 확인 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/modules/backlog/... -run TestExportHistory -v`
Expected: PASS

```bash
git add apex_tools/apex-agent/internal/modules/backlog/export.go apex_tools/apex-agent/internal/modules/backlog/export_test.go
git commit -m "feat(tools): ExportHistory — RESOLVED 항목 HISTORY 자동 이관"
git push
```

---

### Task 2: SyncExport HISTORY 쓰기 추가

**Files:**
- Modify: `internal/workflow/sync.go`
- Modify: `internal/workflow/sync_test.go`

- [ ] **Step 1: SyncExport HISTORY 테스트 추가**

`sync_test.go`에 추가:
```go
func TestSyncExport_WritesHistory(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	// 항목 추가 + resolve
	mgr.Add(&backlog.BacklogItem{
		ID: 1, Title: "해결됨", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(1, "FIXED")

	// 기존 HISTORY 파일 생성
	historyPath := filepath.Join(dir, "docs", "BACKLOG_HISTORY.md")
	os.WriteFile(historyPath, []byte("# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n"), 0o644)

	_, err := SyncExport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}

	// HISTORY 파일에 resolved 항목이 있는지
	data, _ := os.ReadFile(historyPath)
	if !strings.Contains(string(data), "해결됨") {
		t.Error("HISTORY should contain resolved item")
	}
}
```

- [ ] **Step 2: SyncExport 수정 — HISTORY 쓰기 추가**

`sync.go`의 `SyncExport()` 끝에 추가:
```go
	// HISTORY 쓰기 — RESOLVED 항목 자동 이관
	existingHistory, _ := os.ReadFile(historyPath)
	updatedHistory, err := mgr.ExportHistory(string(existingHistory))
	if err != nil {
		return imported, fmt.Errorf("export history: %w", err)
	}
	if updatedHistory != string(existingHistory) {
		if err := os.WriteFile(historyPath, []byte(updatedHistory), 0o644); err != nil {
			return imported, fmt.Errorf("write history: %w", err)
		}
		ml.Info("backlog history 갱신 완료")
	}
```

- [ ] **Step 3: 테스트 통과 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -run TestSyncExport -v`
Expected: PASS

```bash
git add apex_tools/apex-agent/internal/workflow/sync.go apex_tools/apex-agent/internal/workflow/sync_test.go
git commit -m "feat(tools): SyncExport HISTORY 자동 이관 — RESOLVED 항목 prepend"
git push
```

---

### Task 3: backlog export CLI 파일 직접 쓰기

**Files:**
- Modify: `internal/cli/backlog_cmd.go`

- [ ] **Step 1: backlogExportCmd 수정**

`backlog_cmd.go`의 `backlogExportCmd()` 수정:
- 기본 동작: `SyncExport()` 호출 (파일 직접 쓰기)
- `--stdout` 플래그: 기존 stdout 출력 유지

```go
func backlogExportCmd() *cobra.Command {
	var stdout bool

	cmd := &cobra.Command{
		Use:   "export",
		Short: "DB → BACKLOG.md + BACKLOG_HISTORY.md 동기화",
		Long: `DB 내용을 docs/BACKLOG.md + BACKLOG_HISTORY.md에 직접 씁니다.
--stdout 플래그로 기존처럼 stdout 출력할 수 있습니다 (디버깅용).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			if stdout {
				// 기존 stdout 출력 (SafeExport)
				_, mgr, cleanup, err := openBacklogStore()
				if err != nil {
					return err
				}
				defer cleanup()

				root, err := projectRoot()
				if err != nil {
					return fmt.Errorf("프로젝트 루트를 찾을 수 없습니다: %w", err)
				}
				backlogPath := filepath.Join(root, "docs", "BACKLOG.md")
				historyPath := filepath.Join(root, "docs", "BACKLOG_HISTORY.md")
				backlogData, _ := os.ReadFile(backlogPath)
				historyData, _ := os.ReadFile(historyPath)

				content, imported, err := mgr.SafeExport(string(backlogData), string(historyData))
				if err != nil {
					return err
				}
				if imported > 0 {
					fmt.Fprintf(os.Stderr, "[export] import-first: %d items synced\n", imported)
				}
				fmt.Fprint(os.Stdout, content)
				return nil
			}

			// 기본: 파일 직접 쓰기 (SyncExport)
			_, mgr, cleanup, err := openBacklogStore()
			if err != nil {
				return err
			}
			defer cleanup()

			root, err := projectRoot()
			if err != nil {
				return fmt.Errorf("프로젝트 루트를 찾을 수 없습니다: %w", err)
			}

			n, err := workflow.SyncExport(root, mgr)
			if err != nil {
				return err
			}
			fmt.Printf("[export] docs/BACKLOG.md + BACKLOG_HISTORY.md 갱신 완료 (import: %d)\n", n)
			return nil
		},
	}

	cmd.Flags().BoolVar(&stdout, "stdout", false, "stdout 출력 (디버깅용, 파일 쓰기 안 함)")
	return cmd
}
```

import에 `"path/filepath"` 추가, `"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"` 추가.
기존 `--backlog`, `--history`, `--unsafe` 플래그 제거.

- [ ] **Step 2: 빌드 확인 + 커밋**

Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`
Expected: 성공

```bash
git add apex_tools/apex-agent/internal/cli/backlog_cmd.go
git commit -m "refactor(tools): backlog export → 파일 직접 쓰기 기본, --stdout 디버깅"
git push
```

---

### Task 4: backlog update CLI + Manager.Update()

**Files:**
- Modify: `internal/modules/backlog/manage.go`
- Modify: `internal/modules/backlog/module.go`
- Modify: `internal/cli/backlog_cmd.go`

- [ ] **Step 1: Manager.Update 테스트 작성**

기존 `manage_test.go` 또는 `export_test.go`에 추가:
```go
func TestUpdate_SingleField(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "원래 제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "원래 설명",
	})

	err := mgr.Update(1, map[string]string{"title": "수정된 제목"})
	if err != nil {
		t.Fatalf("Update: %v", err)
	}

	items, _ := mgr.List(ListFilter{Status: "OPEN"})
	if len(items) != 1 || items[0].Title != "수정된 제목" {
		t.Errorf("expected '수정된 제목', got '%s'", items[0].Title)
	}
	// 다른 필드 유지 확인
	if items[0].Severity != "MAJOR" {
		t.Errorf("severity should be preserved, got '%s'", items[0].Severity)
	}
}

func TestUpdate_MultipleFields(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(1, map[string]string{
		"severity":    "MINOR",
		"description": "수정된 설명",
	})
	if err != nil {
		t.Fatalf("Update: %v", err)
	}

	items, _ := mgr.List(ListFilter{Status: "OPEN"})
	if items[0].Severity != "MINOR" || items[0].Description != "수정된 설명" {
		t.Errorf("fields not updated: severity=%s desc=%s", items[0].Severity, items[0].Description)
	}
}

func TestUpdate_NotFound(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	err := mgr.Update(999, map[string]string{"title": "x"})
	if err == nil {
		t.Fatal("expected error for non-existent item")
	}
}

func TestUpdate_InvalidEnum(t *testing.T) {
	mgr, cleanup := setupExportTest(t)
	defer cleanup()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(1, map[string]string{"severity": "INVALID"})
	if err == nil {
		t.Fatal("expected error for invalid severity")
	}
}
```

- [ ] **Step 2: Manager.Update 구현**

`manage.go`에 추가:
```go
// allowedUpdateFields maps CLI flag names to DB column names.
var allowedUpdateFields = map[string]string{
	"title":       "title",
	"severity":    "severity",
	"timeframe":   "timeframe",
	"scope":       "scope",
	"type":        "type",
	"description": "description",
	"related":     "related",
}

// Update modifies specified fields of an existing item.
// Only fields present in the map are updated; others are preserved.
func (m *Manager) Update(id int, fields map[string]string) error {
	if len(fields) == 0 {
		return fmt.Errorf("최소 1개 필드를 지정해야 합니다")
	}

	// 존재 확인
	exists, _, err := m.Check(id)
	if err != nil {
		return err
	}
	if !exists {
		return fmt.Errorf("backlog item %d not found", id)
	}

	// 열거형 검증
	if v, ok := fields["severity"]; ok {
		if err := ValidateSeverity(v); err != nil {
			return err
		}
	}
	if v, ok := fields["timeframe"]; ok {
		if err := ValidateTimeframe(v); err != nil {
			return err
		}
	}
	if v, ok := fields["type"]; ok {
		if err := ValidateType(v); err != nil {
			return err
		}
	}

	// 동적 SQL 생성
	var setClauses []string
	var args []any
	for field, value := range fields {
		col, ok := allowedUpdateFields[field]
		if !ok {
			return fmt.Errorf("unknown field: %s", field)
		}
		setClauses = append(setClauses, col+" = ?")
		args = append(args, value)
	}
	setClauses = append(setClauses, "updated_at = datetime('now','localtime')")
	args = append(args, id)

	query := fmt.Sprintf("UPDATE backlog_items SET %s WHERE id = ?", strings.Join(setClauses, ", "))
	_, err = m.q.Exec(query, args...)
	if err != nil {
		return fmt.Errorf("Update #%d: %w", id, err)
	}
	ml.Info("item updated", "id", id, "fields", len(fields))
	return nil
}
```

- [ ] **Step 3: module.go에 라우트 등록 + CLI 커맨드 추가**

`module.go` RegisterRoutes에 `reg.Handle("update", m.handleUpdate)` 추가.
`module.go`에 `handleUpdate` 핸들러 추가.

`backlog_cmd.go`에 `backlogUpdateCmd()` 추가:
```go
func backlogUpdateCmd() *cobra.Command {
	var title, severity, timeframe, scope, itemType, description, related string

	cmd := &cobra.Command{
		Use:   "update ID",
		Short: "백로그 항목 메타데이터 수정",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			var id int
			if _, err := fmt.Sscanf(args[0], "%d", &id); err != nil {
				return fmt.Errorf("ID must be an integer: %s", args[0])
			}

			fields := make(map[string]string)
			if cmd.Flags().Changed("title") { fields["title"] = title }
			if cmd.Flags().Changed("severity") { fields["severity"] = strings.ToUpper(severity) }
			if cmd.Flags().Changed("timeframe") { fields["timeframe"] = strings.ToUpper(strings.ReplaceAll(timeframe, " ", "_")) }
			if cmd.Flags().Changed("scope") { fields["scope"] = scope }
			if cmd.Flags().Changed("type") { fields["type"] = strings.ToUpper(strings.ReplaceAll(itemType, "-", "_")) }
			if cmd.Flags().Changed("description") { fields["description"] = description }
			if cmd.Flags().Changed("related") { fields["related"] = related }

			if len(fields) == 0 {
				return fmt.Errorf("최소 1개 필드를 지정하세요 (--title, --severity, --description 등)")
			}

			_, mgr, cleanup, err := openBacklogStore()
			if err != nil {
				return err
			}
			defer cleanup()

			if err := mgr.Update(id, fields); err != nil {
				return err
			}
			fmt.Printf("Updated #%d: %d fields\n", id, len(fields))
			return nil
		},
	}

	cmd.Flags().StringVar(&title, "title", "", "제목")
	cmd.Flags().StringVar(&severity, "severity", "", "등급 (CRITICAL/MAJOR/MINOR)")
	cmd.Flags().StringVar(&timeframe, "timeframe", "", "시간축 (NOW/IN_VIEW/DEFERRED)")
	cmd.Flags().StringVar(&scope, "scope", "", "스코프")
	cmd.Flags().StringVar(&itemType, "type", "", "타입 (BUG/DESIGN_DEBT/...)")
	cmd.Flags().StringVar(&description, "description", "", "설명")
	cmd.Flags().StringVar(&related, "related", "", "연관 (예: 150,151)")

	return cmd
}
```

`backlogCmd()`에 `cmd.AddCommand(backlogUpdateCmd())` 추가.

- [ ] **Step 4: 테스트 통과 + 빌드 확인 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/modules/backlog/... -run TestUpdate -v`
Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`
Expected: PASS + 빌드 성공

```bash
git add apex_tools/apex-agent/internal/modules/backlog/manage.go apex_tools/apex-agent/internal/modules/backlog/module.go apex_tools/apex-agent/internal/modules/backlog/export_test.go apex_tools/apex-agent/internal/cli/backlog_cmd.go
git commit -m "feat(tools): backlog update CLI — 필드별 개별 메타 수정"
git push
```

---

### Task 5: validate-backlog hook

**Files:**
- Modify: `internal/modules/hook/gate.go`
- Modify: `internal/modules/hook/gate_test.go`
- Modify: `internal/cli/hook_cmd.go`
- Modify: `.claude/settings.json`

- [ ] **Step 1: ValidateBacklog 테스트 작성**

`gate_test.go`에 추가:
```go
func TestValidateBacklog_BlocksBacklogMD(t *testing.T) {
	err := ValidateBacklog("/project/docs/BACKLOG.md")
	if err == nil {
		t.Fatal("expected block for BACKLOG.md")
	}
}

func TestValidateBacklog_BlocksHistoryMD(t *testing.T) {
	err := ValidateBacklog("/project/docs/BACKLOG_HISTORY.md")
	if err == nil {
		t.Fatal("expected block for BACKLOG_HISTORY.md")
	}
}

func TestValidateBacklog_AllowsOtherFiles(t *testing.T) {
	err := ValidateBacklog("/project/docs/CLAUDE.md")
	if err != nil {
		t.Errorf("should allow other files, got: %v", err)
	}
}

func TestValidateBacklog_AllowsSubdirBacklog(t *testing.T) {
	// docs/apex_tools/BACKLOG.md 같은 건 허용 (docs/BACKLOG.md만 차단)
	err := ValidateBacklog("/project/docs/apex_tools/BACKLOG.md")
	if err != nil {
		t.Errorf("should allow subdirectory backlog, got: %v", err)
	}
}
```

- [ ] **Step 2: ValidateBacklog 구현**

`gate.go`에 추가:
```go
// ValidateBacklog blocks direct editing of docs/BACKLOG.md and docs/BACKLOG_HISTORY.md.
// All backlog modifications must go through CLI (backlog add/update/resolve/release/export).
func ValidateBacklog(filePath string) error {
	normalized := strings.ReplaceAll(filePath, "\\", "/")
	if strings.HasSuffix(normalized, "/docs/BACKLOG.md") ||
		strings.HasSuffix(normalized, "/docs/BACKLOG_HISTORY.md") {
		return fmt.Errorf("차단: BACKLOG 파일 직접 편집 금지.\n" +
			"  backlog add/update/resolve/release/export CLI를 사용하세요")
	}
	return nil
}
```

- [ ] **Step 3: hook_cmd.go에 서브커맨드 추가**

```go
func hookValidateBacklogCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-backlog",
		Short: "BACKLOG 파일 직접 편집 차단",
		RunE: func(cmd *cobra.Command, args []string) error {
			filePath, _, err := readHandoffProbeInput()
			if err != nil {
				return nil // parse error → allow
			}
			if err := hook.ValidateBacklog(filePath); err != nil {
				fmt.Fprintln(os.Stderr, err.Error())
				os.Exit(2)
			}
			return nil
		},
	}
}
```

`hookCmd()`에 `cmd.AddCommand(hookValidateBacklogCmd())` 추가.

- [ ] **Step 4: settings.json에 hook 등록**

`Edit|Write` matcher에 추가:
```json
{
  "type": "command",
  "command": "bash ./apex_tools/apex-agent/run-hook hook validate-backlog",
  "timeout": 15
}
```

- [ ] **Step 5: 테스트 + 빌드 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/modules/hook/... -run TestValidateBacklog -v`
Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`

```bash
git add apex_tools/apex-agent/internal/modules/hook/gate.go apex_tools/apex-agent/internal/modules/hook/gate_test.go apex_tools/apex-agent/internal/cli/hook_cmd.go .claude/settings.json
git commit -m "feat(tools): validate-backlog hook — BACKLOG 파일 직접 편집 무조건 차단"
git push
```

---

### Task 6: MergePipeline 순서 재배치

**Files:**
- Modify: `internal/workflow/pipeline.go`
- Modify: `internal/workflow/pipeline_test.go`

- [ ] **Step 1: MergePipeline 수정**

```go
func MergePipeline(params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) error {

	// Phase 1: rebase
	if msg, err := RebaseOnMain(projectRoot); err != nil {
		return err
	} else if msg != "" {
		fmt.Println(msg)
	}

	// Phase 2: import (rebase 후 최신 MD → DB)
	if mgr != nil {
		if _, err := SyncImport(projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog import 실패: %w", err)
		}
	}

	// Phase 3: export (DB → MD + HISTORY)
	if mgr != nil {
		if _, err := SyncExport(projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog export 실패: %w", err)
		}
	}

	// Phase 4: auto-commit + push (export 결과)
	if err := autoCommitExport(projectRoot); err != nil {
		return fmt.Errorf("export 결과 커밋 실패: %w", err)
	}

	// Phase 5: checkout main
	if err := CheckoutMain(projectRoot); err != nil {
		return fmt.Errorf("checkout main 실패: %w", err)
	}

	// Phase 6: IPC notify-merge (마지막 — active에서 삭제)
	if _, err := ipcFn("notify-merge", params); err != nil {
		return fmt.Errorf("notify-merge 실패: %w", err)
	}

	return nil
}

// autoCommitExport stages and commits backlog export results.
// No-op if there are no changes to commit.
func autoCommitExport(projectRoot string) error {
	// Stage
	exec.Command("git", "-C", projectRoot, "add",
		"docs/BACKLOG.md", "docs/BACKLOG_HISTORY.md").Run()

	// Check if anything staged
	if err := exec.Command("git", "-C", projectRoot,
		"diff", "--cached", "--quiet").Run(); err == nil {
		return nil // nothing to commit
	}

	// Commit
	if out, err := exec.Command("git", "-C", projectRoot,
		"commit", "-m", "docs: backlog export (auto-sync)").CombinedOutput(); err != nil {
		return fmt.Errorf("git commit: %w\n%s", err, out)
	}

	// Push
	if out, err := exec.Command("git", "-C", projectRoot,
		"push").CombinedOutput(); err != nil {
		return fmt.Errorf("git push: %w\n%s", err, out)
	}

	ml.Info("backlog export 자동 커밋+푸시 완료")
	return nil
}
```

`pipeline.go` import에 `"os/exec"` 추가.

- [ ] **Step 2: CLI notifyMergeCmd 단순화**

`handoff_cmd.go`의 `notifyMergeCmd`에서 안내 메시지 제거 (이제 MergePipeline이 직접 커밋):
```go
if err := workflow.MergePipeline(params, root, mgr, ipcWrapper); err != nil {
    return err
}
fmt.Printf("[handoff] branch merged (branch=%s)\n", branch)
return nil
```

기존 안내 메시지 `"docs/BACKLOG.md 갱신됨 — 커밋+푸시 후 머지 진행하세요"` 삭제.

- [ ] **Step 3: pipeline_test.go 순서 검증 테스트 수정**

mockIPC의 calls 기록을 확인하여 IPC가 마지막에 호출되는지 검증.
기존 `TestMergePipeline_OK`가 이미 IPC 호출을 확인하므로, 순서는 auto-commit이 추가된 것만 확인.

- [ ] **Step 4: 전체 테스트 + 커밋**

Run: `cd apex_tools/apex-agent && go test ./internal/workflow/... -v -count=1`
Run: `cd apex_tools/apex-agent && go build ./cmd/apex-agent`

```bash
git add apex_tools/apex-agent/internal/workflow/pipeline.go apex_tools/apex-agent/internal/workflow/pipeline_test.go apex_tools/apex-agent/internal/cli/handoff_cmd.go
git commit -m "refactor(tools): MergePipeline 순서 재배치 — commit→checkout→IPC(마지막)"
git push
```

---

### Task 7: CLAUDE.md 보정 + 가이드

**Files:**
- Modify: `apex_tools/apex-agent/CLAUDE.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: apex-agent CLAUDE.md Source of Truth 보정**

데이터 관리 섹션 수정:
```markdown
### 데이터 관리 — Source of Truth

DB: Source of Truth (상태 + 메타데이터)
MD: git 백업 (DB 유실 시 SyncImport로 복원)

- **DB가 전체의 출처** — 상태, 제목, 등급, 설명 등 모든 필드. 상태 변경은 CLI 전용
- **MD는 git 백업** — DB 유실 시 `migrate backlog`으로 복원. export가 DB→MD 전체 덮어쓰기
- **MD 직접 편집 금지** — validate-backlog hook이 차단. `backlog add/update/resolve/release` CLI 사용

Import (`migrate backlog`) — MD → DB:
- DB에 없는 항목 → 새로 추가
- DB에 있는 항목 → 메타데이터 갱신, 상태 불변
- MD에서 삭제된 항목 → DB에 잔존

Export (`backlog export`) — DB → MD 파일 직접 쓰기:
- OPEN + FIXING → docs/BACKLOG.md
- RESOLVED → docs/BACKLOG_HISTORY.md (prepend, 중복 방지)
```

export 사용법 갱신:
```markdown
| 머지 전 (⑥) | `backlog export` | DB → docs/BACKLOG.md + BACKLOG_HISTORY.md 직접 쓰기 |
```

리다이렉션 경고 제거 (더 이상 불필요).

- [ ] **Step 2: 루트 CLAUDE.md 백로그 규칙 추가**

문서/프로세스 규칙 섹션에 추가:
```markdown
- **백로그 파일 직접 편집 금지** — `docs/BACKLOG.md`, `docs/BACKLOG_HISTORY.md`는 `validate-backlog` hook이 차단. `backlog add/update/resolve/release/export` CLI 사용 필수
```

backlog update CLI 사용법 추가:
```markdown
backlog update ID --title "..." --severity MAJOR --description "..."
```

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/CLAUDE.md CLAUDE.md
git commit -m "docs(tools): Source of Truth 보정 + 백로그 직접 편집 금지 가이드"
git push
```

---

### Task 8: 전체 테스트 + 빌드 검증

- [ ] **Step 1: Go 전체 테스트**

Run: `cd apex_tools/apex-agent && go test ./... -count=1 -v`
Expected: ALL PASS

- [ ] **Step 2: 바이너리 설치 + 스모크 테스트**

```bash
apex-agent daemon stop
cp apex-agent.exe "$LOCALAPPDATA/apex-agent/apex-agent.exe"
apex-agent daemon start

# backlog update 동작 확인
apex-agent backlog update 154 --title "테스트"
apex-agent backlog list | grep 154

# backlog export 동작 확인
apex-agent backlog export

# validate-backlog hook 동작 확인 (Edit 시도 → 차단 기대)
```
