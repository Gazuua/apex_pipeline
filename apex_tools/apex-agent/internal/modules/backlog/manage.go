// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("backlog")

// BacklogItem represents a single entry in the backlog_items table.
type BacklogItem struct {
	ID            int    `json:"id"`
	Title         string `json:"title"`
	Severity      string `json:"severity"`             // CRITICAL, MAJOR, MINOR
	Timeframe     string `json:"timeframe"`            // NOW, IN_VIEW, DEFERRED
	Scope         string `json:"scope"`
	Type          string `json:"type"`
	Description   string `json:"description"`
	Related       string `json:"related,omitempty"`
	Position      int    `json:"position"`
	Status        string `json:"status"`               // OPEN, FIXING, RESOLVED
	Resolution    string `json:"resolution,omitempty"`
	ResolvedAt    string `json:"resolved_at,omitempty"`
	BlockedReason string `json:"blocked_reason,omitempty"`
	CreatedAt     string `json:"created_at"`
	UpdatedAt     string `json:"updated_at"`
}

// BacklogJSON is the top-level structure for docs/BACKLOG.json.
type BacklogJSON struct {
	NextID int           `json:"next_id"`
	Items  []BacklogItem `json:"items"`
}

// ListFilter specifies optional filters for List queries.
type ListFilter struct {
	Timeframe string // optional: filter by timeframe
	Severity  string // optional: filter by severity
	Status    string // optional: filter by status; empty string means all open
}

// JunctionCleaner removes the backlog-branch junction record.
// Injected by handoff module to avoid cross-module table access.
type JunctionCleaner func(ctx context.Context, q store.Querier, backlogID int) error

// JunctionCreator creates a backlog-branch junction record.
// Injected by handoff module to avoid cross-module table access.
type JunctionCreator func(ctx context.Context, q store.Querier, branch string, backlogID int) error

// Manager handles CRUD operations on the backlog_items table.
type Manager struct {
	store           *store.Store   // for RunInTx (top-level only)
	q               store.Querier  // for all queries (Store or TxStore)
	junctionCleaner JunctionCleaner
	junctionCreator JunctionCreator
}

// NewManager creates a new Manager backed by the given store.
func NewManager(s *store.Store) *Manager {
	return &Manager{store: s, q: s}
}

// SetJunctionCleaner sets the callback for cleaning up backlog-branch junction records.
// Called by daemon setup after handoff module creation.
func (m *Manager) SetJunctionCleaner(fn JunctionCleaner) {
	m.junctionCleaner = fn
}

// SetJunctionCreator sets the callback for creating backlog-branch junction records.
// Called by daemon setup after handoff module creation.
func (m *Manager) SetJunctionCreator(fn JunctionCreator) {
	m.junctionCreator = fn
}

// withQuerier creates a Manager copy that uses the given Querier for queries.
// Used inside RunInTx to route queries through the transaction.
func (m *Manager) withQuerier(q store.Querier) *Manager {
	return &Manager{store: m.store, q: q, junctionCleaner: m.junctionCleaner, junctionCreator: m.junctionCreator}
}

// NextID returns the next available backlog item ID (max(id)+1, or 1 if empty).
func (m *Manager) NextID(ctx context.Context) (int, error) {
	row := m.q.QueryRow(ctx, "SELECT COALESCE(MAX(id), 0) FROM backlog_items")
	var maxID int
	if err := row.Scan(&maxID); err != nil {
		return 0, fmt.Errorf("NextID: %w", err)
	}
	return maxID + 1, nil
}

// Add inserts a new backlog item. If Position is 0, it is auto-assigned to the
// end of the item's timeframe group (max position in that group + 1).
func (m *Manager) Add(ctx context.Context, item *BacklogItem) error {
	ml.Info("Add begin", "title", item.Title, "severity", item.Severity,
		"timeframe", item.Timeframe, "scope", item.Scope, "type", item.Type)
	if err := ValidateSeverity(item.Severity); err != nil {
		return err
	}
	// Reject empty timeframe at Add level (allowed only via import for history items).
	if item.Timeframe == "" {
		return fmt.Errorf("timeframe is required (use NOW, IN_VIEW, or DEFERRED)")
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

	if item.Position == 0 {
		row := m.q.QueryRow(ctx,
			"SELECT COALESCE(MAX(position), 0) FROM backlog_items WHERE timeframe = ?",
			item.Timeframe,
		)
		var maxPos int
		if err := row.Scan(&maxPos); err != nil {
			return fmt.Errorf("Add: resolve position: %w", err)
		}
		item.Position = maxPos + 1
	}

	status := item.Status
	if status == "" {
		status = StatusOpen
	}

	if item.ID == 0 {
		// AUTOINCREMENT: id 생략 시 SQLite가 자동 할당
		result, err := m.q.Exec(ctx, `
			INSERT INTO backlog_items
				(title, severity, timeframe, scope, type, description, related,
				 position, status, resolution, resolved_at)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			item.Title, item.Severity, item.Timeframe, item.Scope, item.Type,
			item.Description, item.Related, item.Position, status,
			store.NullableString(item.Resolution), store.NullableString(item.ResolvedAt),
		)
		if err != nil {
			return fmt.Errorf("Add: %w", err)
		}
		id, err := result.LastInsertId()
		if err != nil {
			return fmt.Errorf("Add: LastInsertId: %w", err)
		}
		item.ID = int(id)
	} else {
		_, err := m.q.Exec(ctx, `
			INSERT INTO backlog_items
				(id, title, severity, timeframe, scope, type, description, related,
				 position, status, resolution, resolved_at)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			item.ID, item.Title, item.Severity, item.Timeframe, item.Scope, item.Type,
			item.Description, item.Related, item.Position, status,
			store.NullableString(item.Resolution), store.NullableString(item.ResolvedAt),
		)
		if err != nil {
			return fmt.Errorf("Add: %w", err)
		}
	}
	ml.Info("item added", "id", item.ID, "severity", item.Severity, "timeframe", item.Timeframe)
	return nil
}

// scanBacklogItem scans a row into a BacklogItem.
func scanBacklogItem(scanner interface{ Scan(dest ...any) error }) (*BacklogItem, error) {
	var item BacklogItem
	err := scanner.Scan(
		&item.ID, &item.Title, &item.Severity, &item.Timeframe, &item.Scope, &item.Type,
		&item.Description, &item.Related, &item.Position, &item.Status,
		&item.Resolution, &item.ResolvedAt, &item.BlockedReason, &item.CreatedAt, &item.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &item, nil
}

// Get retrieves a single backlog item by ID.
// Returns nil, nil if the item does not exist.
func (m *Manager) Get(ctx context.Context, id int) (*BacklogItem, error) {
	row := m.q.QueryRow(ctx, `
		SELECT id, title, severity, timeframe, scope, type, description,
		       COALESCE(related, ''), position, status,
		       COALESCE(resolution, ''), COALESCE(resolved_at, ''), COALESCE(blocked_reason, ''),
		       created_at, updated_at
		FROM backlog_items WHERE id = ?`, id)

	item, err := scanBacklogItem(row)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("Get: %w", err)
	}
	return item, nil
}

// List retrieves backlog items matching the filter, ordered by timeframe then position.
// Timeframe ordering: NOW < IN_VIEW < DEFERRED.
// If filter.Status is empty, defaults to "OPEN".
func (m *Manager) List(ctx context.Context, filter ListFilter) ([]BacklogItem, error) {
	status := filter.Status
	if status == "" {
		status = StatusOpen
	}

	query := `
		SELECT id, title, severity, timeframe, scope, type, description,
		       COALESCE(related, ''), position, status,
		       COALESCE(resolution, ''), COALESCE(resolved_at, ''), COALESCE(blocked_reason, ''),
		       created_at, updated_at
		FROM backlog_items
		WHERE status = ?`
	args := []any{status}

	if filter.Timeframe != "" {
		query += " AND timeframe = ?"
		args = append(args, filter.Timeframe)
	}
	if filter.Severity != "" {
		query += " AND severity = ?"
		args = append(args, filter.Severity)
	}

	query += `
		ORDER BY
			CASE timeframe
				WHEN 'NOW'      THEN 1
				WHEN 'IN_VIEW'  THEN 2
				WHEN 'DEFERRED' THEN 3
				ELSE 4
			END,
			position ASC`

	rows, err := m.q.Query(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("List: %w", err)
	}
	defer rows.Close()

	var items []BacklogItem
	for rows.Next() {
		item, err := scanBacklogItem(rows)
		if err != nil {
			return nil, fmt.Errorf("List scan: %w", err)
		}
		items = append(items, *item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("List rows: %w", err)
	}
	return items, nil
}

// ListAll retrieves all backlog items ordered for JSON export:
// OPEN items first (NOW → IN_VIEW → DEFERRED, by position),
// then RESOLVED items (by resolved_at DESC).
func (m *Manager) ListAll(ctx context.Context) ([]BacklogItem, error) {
	query := `
		SELECT id, title, severity, timeframe, scope, type, description,
		       COALESCE(related, ''), position, status,
		       COALESCE(resolution, ''), COALESCE(resolved_at, ''), COALESCE(blocked_reason, ''),
		       created_at, updated_at
		FROM backlog_items
		ORDER BY
			CASE status
				WHEN 'OPEN'     THEN 1
				WHEN 'FIXING'   THEN 1
				WHEN 'RESOLVED' THEN 2
				ELSE 3
			END ASC,
			CASE WHEN status != 'RESOLVED' THEN
				CASE timeframe
					WHEN 'NOW'      THEN 1
					WHEN 'IN_VIEW'  THEN 2
					WHEN 'DEFERRED' THEN 3
					ELSE 4
				END
			ELSE 999 END ASC,
			CASE WHEN status != 'RESOLVED' THEN position ELSE 0 END ASC,
			CASE WHEN status = 'RESOLVED' THEN resolved_at ELSE NULL END DESC`

	rows, err := m.q.Query(ctx, query)
	if err != nil {
		return nil, fmt.Errorf("ListAll: %w", err)
	}
	defer rows.Close()

	var items []BacklogItem
	for rows.Next() {
		item, err := scanBacklogItem(rows)
		if err != nil {
			return nil, fmt.Errorf("ListAll scan: %w", err)
		}
		items = append(items, *item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("ListAll rows: %w", err)
	}
	return items, nil
}

// UpdateFromImport updates metadata fields for an existing item from JSON/MD import.
// Only updates (and bumps updated_at) if at least one field actually changed.
// Does NOT touch status/resolution/resolved_at — those are managed exclusively by CLI (Resolve/Release/Fix).
// If importUpdatedAt is non-empty and the DB item's updated_at is strictly newer,
// the import data is considered stale and the update is skipped (DB wins).
func (m *Manager) UpdateFromImport(ctx context.Context, id int, title, severity, timeframe, scope, itemType, description, related string, position int, importUpdatedAt string) error {
	// Read current values to detect changes.
	existing, err := m.Get(ctx, id)
	if err != nil {
		return fmt.Errorf("UpdateFromImport #%d: read existing: %w", id, err)
	}
	if existing == nil {
		return fmt.Errorf("UpdateFromImport #%d: item not found", id)
	}

	// Stale import guard: if DB was updated more recently, skip import.
	if importUpdatedAt != "" && existing.UpdatedAt > importUpdatedAt {
		ml.Info("import skipped (DB newer)", "id", id, "db_updated", existing.UpdatedAt, "import_updated", importUpdatedAt)
		return nil
	}

	// Compare all import-managed fields (status excluded — DB owns status).
	if existing.Title == title &&
		existing.Severity == severity &&
		existing.Timeframe == timeframe &&
		existing.Scope == scope &&
		existing.Type == itemType &&
		existing.Description == description &&
		existing.Related == related &&
		existing.Position == position {
		return nil
	}

	_, err = m.q.Exec(ctx, `
		UPDATE backlog_items
		SET title = ?, severity = ?, timeframe = ?, scope = ?, type = ?,
		    description = ?, related = ?, position = ?,
		    updated_at = datetime('now','localtime')
		WHERE id = ?`,
		title, severity, timeframe, scope, itemType,
		description, related, position, id,
	)
	if err != nil {
		return fmt.Errorf("UpdateFromImport #%d: %w", id, err)
	}
	ml.Info("item updated from import", "id", id, "severity", severity, "timeframe", timeframe)
	return nil
}

// Resolve marks an item as resolved with the given resolution type.
// Returns an error if the item does not exist or is already RESOLVED.
func (m *Manager) Resolve(ctx context.Context, id int, resolution string) error {
	ml.Info("Resolve begin", "id", id, "resolution", resolution)
	if err := ValidateResolution(resolution); err != nil {
		return err
	}
	result, err := m.q.Exec(ctx, `
		UPDATE backlog_items
		SET status = ?,
		    resolution = ?,
		    resolved_at = datetime('now','localtime'),
		    updated_at = datetime('now','localtime')
		WHERE id = ? AND status != ?`,
		StatusResolved, resolution, id, StatusResolved,
	)
	if err != nil {
		return fmt.Errorf("Resolve: %w", err)
	}
	n, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("Resolve rows affected: %w", err)
	}
	if n == 0 {
		return fmt.Errorf("Resolve: item %d not found or already RESOLVED", id)
	}
	ml.Info("item resolved", "id", id, "resolution", resolution)
	return nil
}

// SetStatus updates the status of a backlog item.
func (m *Manager) SetStatus(ctx context.Context, id int, status string) error {
	return m.SetStatusWith(ctx, m.q, id, status)
}

// SetStatusWith updates the status of a backlog item using the provided store
// (which may be a transaction-bound copy from RunInTx).
// DB 레벨 가드: FIXING 전이 시 이미 FIXING이면 차단, OPEN 전이 시 FIXING만 허용 (RESOLVED 원복 방지).
func (m *Manager) SetStatusWith(ctx context.Context, q store.Querier, id int, status string) error {
	if err := ValidateStatus(status); err != nil {
		return err
	}

	query := `UPDATE backlog_items SET status = ?, updated_at = datetime('now','localtime') WHERE id = ?`
	args := []any{status, id}

	switch status {
	case StatusFixing:
		// FIXING 전이는 OPEN에서만 허용 — RESOLVED→FIXING, FIXING→FIXING 원복 방지 (TOCTOU 해소)
		query = `UPDATE backlog_items SET status = ?, updated_at = datetime('now','localtime') WHERE id = ? AND status = ?`
		args = append(args, StatusOpen)
	case StatusOpen:
		// OPEN 전이는 FIXING에서만 허용 — RESOLVED 원복 방지
		query = `UPDATE backlog_items SET status = ?, updated_at = datetime('now','localtime') WHERE id = ? AND status = ?`
		args = append(args, StatusFixing)
	}

	result, err := q.Exec(ctx, query, args...)
	if err != nil {
		return fmt.Errorf("SetStatus: %w", err)
	}
	n, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("SetStatus RowsAffected: %w", err)
	}
	if n == 0 {
		switch status {
		case StatusFixing:
			// Query current state via the same Querier (may be TX) for precise diagnostics.
			var currentStatus string
			scanErr := q.QueryRow(ctx, `SELECT status FROM backlog_items WHERE id = ?`, id).Scan(&currentStatus)
			if errors.Is(scanErr, sql.ErrNoRows) {
				return fmt.Errorf("SetStatus: item %d not found", id)
			}
			if scanErr != nil {
				// Fallback to generic message if diagnostic query fails.
				return fmt.Errorf("SetStatus: item %d not found or not OPEN", id)
			}
			if currentStatus == StatusFixing {
				return fmt.Errorf("SetStatus: item %d already FIXING (possibly concurrent registration)", id)
			}
			if currentStatus == StatusResolved {
				return fmt.Errorf("SetStatus: item %d is RESOLVED (resolved items cannot transition to FIXING)", id)
			}
			return fmt.Errorf("SetStatus: item %d unexpected state %s for FIXING transition (expected OPEN)", id, currentStatus)
		case StatusOpen:
			return fmt.Errorf("SetStatus: item %d not found or not FIXING (RESOLVED items cannot revert to OPEN)", id)
		default:
			return fmt.Errorf("SetStatus: item %d not found", id)
		}
	}
	ml.Info("status changed", "id", id, "status", status)
	return nil
}

// Release removes a backlog item from active work.
// If status is FIXING, sets it back to OPEN and appends release reason to description.
// Non-FIXING items are rejected to prevent accidental description pollution.
func (m *Manager) Release(ctx context.Context, id int, reason, branch string) error {
	ml.Info("Release begin", "id", id, "branch", branch, "reason", reason)
	return m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		txm := m.withQuerier(tx)

		item, err := txm.Get(ctx, id)
		if err != nil {
			return fmt.Errorf("Release: %w", err)
		}
		if item == nil {
			return fmt.Errorf("Release: item %d not found", id)
		}
		if item.Status != StatusFixing {
			return fmt.Errorf("Release: item %d is %s (only FIXING items can be released)", id, item.Status)
		}

		// Append release history to description + set OPEN
		appendDesc := fmt.Sprintf("\n[RELEASED] %s: %s", branch, reason)
		_, err = tx.Exec(ctx, `
			UPDATE backlog_items
			SET status = ?,
			    description = description || ?,
			    updated_at = datetime('now','localtime')
			WHERE id = ?`,
			StatusOpen, appendDesc, id,
		)
		if err != nil {
			return fmt.Errorf("Release: %w", err)
		}

		// Junction 정리: handoff 모듈이 주입한 콜백으로 branch_backlogs 레코드 삭제.
		// 콜백 미설정 시(테스트 등) noop.
		if m.junctionCleaner != nil {
			if delErr := m.junctionCleaner(ctx, tx, id); delErr != nil {
				ml.Warn("failed to delete branch_backlogs on release", "backlog_id", id, "err", delErr)
			}
		}

		ml.Info("item released", "id", id, "reason", reason)
		return nil
	})
}

// Fix links a backlog item to the current branch and transitions it to FIXING.
// Only OPEN items can be fixed. Already FIXING items are silently accepted.
// Check + update + junction insert are wrapped in a transaction for atomicity.
func (m *Manager) Fix(ctx context.Context, id int, branch string) error {
	ml.Info("Fix begin", "id", id, "branch", branch)
	return m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		txm := m.withQuerier(tx)

		item, err := txm.Get(ctx, id)
		if err != nil {
			return fmt.Errorf("Fix: %w", err)
		}
		if item == nil {
			return fmt.Errorf("Fix: item %d not found", id)
		}
		if item.Status == StatusFixing {
			return nil // already FIXING — idempotent
		}
		if item.Status != StatusOpen {
			return fmt.Errorf("Fix: item %d is %s (only OPEN items can be fixed)", id, item.Status)
		}

		// FIXING 전이
		if _, err := tx.Exec(ctx,
			`UPDATE backlog_items SET status = ?, updated_at = datetime('now','localtime') WHERE id = ?`,
			StatusFixing, id,
		); err != nil {
			return fmt.Errorf("Fix: update status: %w", err)
		}

		// Junction 생성: handoff 모듈이 주입한 콜백으로 branch_backlogs 레코드 삽입.
		// 콜백 미설정 시(테스트 등) noop.
		if m.junctionCreator != nil {
			if err := m.junctionCreator(ctx, tx, branch, id); err != nil {
				return fmt.Errorf("Fix: link branch: %w", err)
			}
		}

		ml.Info("item fixed", "id", id, "branch", branch)
		return nil
	})
}

// Check returns whether a backlog item exists and its current status.
// Returns exists=false, status="", nil if the item does not exist.
func (m *Manager) Check(ctx context.Context, id int) (exists bool, status string, err error) {
	row := m.q.QueryRow(ctx, "SELECT status FROM backlog_items WHERE id = ?", id)
	err = row.Scan(&status)
	if errors.Is(err, sql.ErrNoRows) {
		return false, "", nil
	}
	if err != nil {
		return false, "", fmt.Errorf("Check: %w", err)
	}
	return true, status, nil
}

// allowedUpdateFields maps CLI flag names to DB column names.
var allowedUpdateFields = map[string]string{
	"title":       "title",
	"severity":    "severity",
	"timeframe":   "timeframe",
	"scope":       "scope",
	"type":        "type",
	"description": "description",
	"related":     "related",
	"position":    "position",
	"blocked":     "blocked_reason",
}

// Update modifies specified fields of an existing item.
// Only fields present in the map are updated; others are preserved.
func (m *Manager) Update(ctx context.Context, id int, fields map[string]string) error {
	ml.Info("Update begin", "id", id, "field_count", len(fields))
	if len(fields) == 0 {
		return fmt.Errorf("최소 1개 필드를 지정해야 합니다")
	}

	exists, _, err := m.Check(ctx, id)
	if err != nil {
		return err
	}
	if !exists {
		return fmt.Errorf("backlog item %d not found", id)
	}

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
	if v, ok := fields["scope"]; ok {
		if err := ValidateScope(v); err != nil {
			return err
		}
	}

	// position 변경 사전 검증 (트랜잭션 진입 전에 파싱)
	hasPosition := false
	var newPos int
	if newPosStr, ok := fields["position"]; ok {
		var convErr error
		newPos, convErr = strconv.Atoi(newPosStr)
		if convErr != nil || newPos < 1 {
			return fmt.Errorf("position must be a positive integer: %s", newPosStr)
		}
		hasPosition = true
	}

	// Sort field names for deterministic SQL generation (aids debugging).
	fieldNames := make([]string, 0, len(fields))
	for field := range fields {
		fieldNames = append(fieldNames, field)
	}
	sort.Strings(fieldNames)

	var setClauses []string
	var args []any
	for _, field := range fieldNames {
		col, ok := allowedUpdateFields[field]
		if !ok {
			return fmt.Errorf("unknown field: %s", field)
		}
		setClauses = append(setClauses, col+" = ?")
		args = append(args, fields[field])
	}
	setClauses = append(setClauses, "updated_at = datetime('now','localtime')")
	args = append(args, id)

	query := fmt.Sprintf("UPDATE backlog_items SET %s WHERE id = ?", strings.Join(setClauses, ", "))

	// position 재배치가 포함된 경우 트랜잭션으로 감싸서 원자적 실행 보장
	if hasPosition {
		return m.store.RunInTx(ctx, func(tx *store.TxStore) error {
			txm := m.withQuerier(tx)

			// timeframe 동시 변경 여부 확인
			targetTimeframe := ""
			if tf, tfOK := fields["timeframe"]; tfOK {
				targetTimeframe = tf
			} else {
				item, getErr := txm.Get(ctx, id)
				if getErr != nil {
					return getErr
				}
				targetTimeframe = item.Timeframe
			}
			// 같은 timeframe 내에서 newPos 이상인 항목들의 position을 +1
			if _, shiftErr := tx.Exec(ctx, `
				UPDATE backlog_items
				SET position = position + 1
				WHERE timeframe = ? AND position >= ? AND id != ? AND status != ?`,
				targetTimeframe, newPos, id, StatusResolved); shiftErr != nil {
				return fmt.Errorf("reorder position: %w", shiftErr)
			}

			// 메인 UPDATE
			if _, execErr := tx.Exec(ctx, query, args...); execErr != nil {
				return fmt.Errorf("Update #%d: %w", id, execErr)
			}
			ml.Info("item updated", "id", id, "fields", len(fields))
			return nil
		})
	}

	// position 변경 없는 경우 — 단독 쿼리
	_, err = m.q.Exec(ctx, query, args...)
	if err != nil {
		return fmt.Errorf("Update #%d: %w", id, err)
	}
	ml.Info("item updated", "id", id, "fields", len(fields))
	return nil
}

// ListFixingForBranch returns backlog IDs from the given list that have status FIXING.
// Used by handoff merge gate to check for unresolved backlogs.
func (m *Manager) ListFixingForBranch(ctx context.Context, branch string, backlogIDs []int) ([]int, error) {
	if len(backlogIDs) == 0 {
		return nil, nil
	}

	// Build dynamic placeholders: (?, ?, ...)
	placeholders := make([]string, len(backlogIDs))
	args := make([]any, len(backlogIDs))
	for i, id := range backlogIDs {
		placeholders[i] = "?"
		args[i] = id
	}

	query := fmt.Sprintf(
		"SELECT id FROM backlog_items WHERE id IN (%s) AND status = ?",
		strings.Join(placeholders, ", "),
	)
	args = append(args, StatusFixing)

	rows, err := m.q.Query(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("ListFixingForBranch: %w", err)
	}
	defer rows.Close()

	var fixing []int
	for rows.Next() {
		var id int
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("ListFixingForBranch scan: %w", err)
		}
		fixing = append(fixing, id)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("ListFixingForBranch rows: %w", err)
	}
	return fixing, nil
}

// ── Dashboard queries ─────────────────────────────────────────────────────────

// DashboardStatusCounts returns the count of items per status (OPEN, FIXING, RESOLVED).
func (m *Manager) DashboardStatusCounts(ctx context.Context) (map[string]int, error) {
	rows, err := m.q.Query(ctx, `SELECT status, COUNT(*) FROM backlog_items GROUP BY status`)
	if err != nil {
		return nil, fmt.Errorf("DashboardStatusCounts: %w", err)
	}
	defer rows.Close()
	counts := make(map[string]int)
	for rows.Next() {
		var status string
		var count int
		if err := rows.Scan(&status, &count); err != nil {
			return nil, fmt.Errorf("DashboardStatusCounts scan: %w", err)
		}
		counts[status] = count
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("DashboardStatusCounts rows: %w", err)
	}
	return counts, nil
}

// DashboardSeverityCounts returns severity counts for non-resolved items.
func (m *Manager) DashboardSeverityCounts(ctx context.Context) (map[string]int, error) {
	rows, err := m.q.Query(ctx, `SELECT severity, COUNT(*) FROM backlog_items WHERE status != 'RESOLVED' GROUP BY severity`)
	if err != nil {
		return nil, fmt.Errorf("DashboardSeverityCounts: %w", err)
	}
	defer rows.Close()
	counts := make(map[string]int)
	for rows.Next() {
		var sev string
		var count int
		if err := rows.Scan(&sev, &count); err != nil {
			return nil, fmt.Errorf("DashboardSeverityCounts scan: %w", err)
		}
		counts[sev] = count
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("DashboardSeverityCounts rows: %w", err)
	}
	return counts, nil
}

// DashboardItem is a lightweight view for dashboard/API display.
type DashboardItem struct {
	ID            int    `json:"id"`
	Title         string `json:"title"`
	Severity      string `json:"severity"`
	Timeframe     string `json:"timeframe"`
	Scope         string `json:"scope"`
	Type          string `json:"type"`
	Status        string `json:"status"`
	Description   string `json:"description"`
	Related       string `json:"related"`
	Resolution    string `json:"resolution"`
	BlockedReason string `json:"blocked_reason"`
	CreatedAt     string `json:"created_at"`
	UpdatedAt     string `json:"updated_at"`
}

// DashboardFilter specifies optional multi-value filters for dashboard queries.
type DashboardFilter struct {
	Status    []string
	Severity  []string
	Timeframe []string
	Scope     []string
	Type      []string
	SortBy    string
	SortDir   string
}

// DashboardList returns backlog items matching the filter for dashboard display.
// Filter/sort logic mirrors the httpd queries.go behavior.
func (m *Manager) DashboardList(ctx context.Context, f DashboardFilter) ([]DashboardItem, error) {
	var where []string
	var args []any

	addInFilter := func(col string, vals []string) {
		if len(vals) == 0 {
			return
		}
		placeholders := make([]string, len(vals))
		for i, v := range vals {
			placeholders[i] = "?"
			args = append(args, v)
		}
		where = append(where, col+" IN ("+strings.Join(placeholders, ",")+")")
	}
	addInFilter("status", f.Status)
	addInFilter("severity", f.Severity)
	addInFilter("timeframe", f.Timeframe)
	// scope는 쉼표 구분 multi-value ("CORE, SHARED") — LIKE로 부분 매칭 (OR)
	if len(f.Scope) > 0 {
		scopeConds := make([]string, len(f.Scope))
		for i, s := range f.Scope {
			scopeConds[i] = "scope LIKE ?"
			args = append(args, "%"+s+"%")
		}
		where = append(where, "("+strings.Join(scopeConds, " OR ")+")")
	}
	addInFilter("type", f.Type)

	query := `SELECT id, title, severity, timeframe, scope, type, status, description,
	          COALESCE(related,''), COALESCE(resolution,''), COALESCE(blocked_reason,''), created_at, updated_at
	          FROM backlog_items`
	if len(where) > 0 {
		query += " WHERE " + strings.Join(where, " AND ")
	}

	// Sort — default: FIXING first → timeframe urgency → severity → ID
	// SAFETY: sortCol and sortDir are always from hardcoded allowlists below.
	if f.SortBy != "" {
		sortCol := "id"
		allowed := map[string]bool{"id": true, "severity": true, "created_at": true, "updated_at": true, "status": true}
		if allowed[f.SortBy] {
			sortCol = f.SortBy
		}
		sortDir := "DESC"
		if f.SortDir == "ASC" {
			sortDir = "ASC"
		}
		query += " ORDER BY " + sortCol + " " + sortDir
	} else {
		query += ` ORDER BY
			CASE status WHEN 'FIXING' THEN 0 WHEN 'OPEN' THEN 1 ELSE 2 END,
			CASE timeframe WHEN 'NOW' THEN 0 WHEN 'IN_VIEW' THEN 1 WHEN 'DEFERRED' THEN 2 ELSE 3 END,
			CASE severity WHEN 'CRITICAL' THEN 0 WHEN 'MAJOR' THEN 1 WHEN 'MINOR' THEN 2 ELSE 3 END,
			id DESC`
	}

	rows, err := m.q.Query(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("DashboardList: %w", err)
	}
	defer rows.Close()

	var items []DashboardItem
	for rows.Next() {
		var b DashboardItem
		if err := rows.Scan(&b.ID, &b.Title, &b.Severity, &b.Timeframe, &b.Scope, &b.Type,
			&b.Status, &b.Description, &b.Related, &b.Resolution, &b.BlockedReason, &b.CreatedAt, &b.UpdatedAt); err != nil {
			return nil, fmt.Errorf("DashboardList scan: %w", err)
		}
		items = append(items, b)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("DashboardList rows: %w", err)
	}
	return items, nil
}

// DashboardGetByID returns a single backlog item for inline display.
func (m *Manager) DashboardGetByID(ctx context.Context, id int) (*DashboardItem, error) {
	row := m.q.QueryRow(ctx, `SELECT id, title, severity, timeframe, scope, type, status, description,
		COALESCE(related,''), COALESCE(resolution,''), COALESCE(blocked_reason,''), created_at, updated_at
		FROM backlog_items WHERE id = ?`, id)
	var b DashboardItem
	err := row.Scan(&b.ID, &b.Title, &b.Severity, &b.Timeframe, &b.Scope, &b.Type,
		&b.Status, &b.Description, &b.Related, &b.Resolution, &b.BlockedReason, &b.CreatedAt, &b.UpdatedAt)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, nil
		}
		return nil, fmt.Errorf("DashboardGetByID: %w", err)
	}
	return &b, nil
}

// DashboardBlockedCount returns the number of FIXING items with a non-empty blocked_reason.
func (m *Manager) DashboardBlockedCount(ctx context.Context) (int, error) {
	var count int
	err := m.q.QueryRow(ctx,
		`SELECT COUNT(*) FROM backlog_items WHERE status = 'FIXING' AND blocked_reason IS NOT NULL AND blocked_reason != ''`,
	).Scan(&count)
	return count, err
}
