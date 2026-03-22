// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("backlog")

// BacklogItem represents a single entry in the backlog_items table.
type BacklogItem struct {
	ID          int
	Title       string
	Severity    string // CRITICAL, MAJOR, MINOR
	Timeframe   string // NOW, IN_VIEW, DEFERRED
	Scope       string
	Type        string
	Description string
	Related     string
	Position    int
	Status      string // OPEN, FIXING, RESOLVED
	Resolution  string
	ResolvedAt  string
	CreatedAt   string
	UpdatedAt   string
}

// ListFilter specifies optional filters for List queries.
type ListFilter struct {
	Timeframe string // optional: filter by timeframe
	Severity  string // optional: filter by severity
	Status    string // optional: filter by status; empty string means all open
}

// Manager handles CRUD operations on the backlog_items table.
type Manager struct {
	store *store.Store
}

// NewManager creates a new Manager backed by the given store.
func NewManager(s *store.Store) *Manager {
	return &Manager{store: s}
}

// NextID returns the next available backlog item ID (max(id)+1, or 1 if empty).
func (m *Manager) NextID() (int, error) {
	row := m.store.QueryRow("SELECT COALESCE(MAX(id), 0) FROM backlog_items")
	var maxID int
	if err := row.Scan(&maxID); err != nil {
		return 0, fmt.Errorf("NextID: %w", err)
	}
	return maxID + 1, nil
}

// Add inserts a new backlog item. If Position is 0, it is auto-assigned to the
// end of the item's timeframe group (max position in that group + 1).
func (m *Manager) Add(item *BacklogItem) error {
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

	if item.Position == 0 {
		row := m.store.QueryRow(
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
		result, err := m.store.Exec(`
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
		id, _ := result.LastInsertId()
		item.ID = int(id)
	} else {
		_, err := m.store.Exec(`
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
		&item.Resolution, &item.ResolvedAt, &item.CreatedAt, &item.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &item, nil
}

// Get retrieves a single backlog item by ID.
// Returns nil, nil if the item does not exist.
func (m *Manager) Get(id int) (*BacklogItem, error) {
	row := m.store.QueryRow(`
		SELECT id, title, severity, timeframe, scope, type, description,
		       COALESCE(related, ''), position, status,
		       COALESCE(resolution, ''), COALESCE(resolved_at, ''),
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
func (m *Manager) List(filter ListFilter) ([]BacklogItem, error) {
	status := filter.Status
	if status == "" {
		status = StatusOpen
	}

	query := `
		SELECT id, title, severity, timeframe, scope, type, description,
		       COALESCE(related, ''), position, status,
		       COALESCE(resolution, ''), COALESCE(resolved_at, ''),
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

	rows, err := m.store.Query(query, args...)
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

// UpdateFromImport updates metadata fields for an existing item from MD import.
// The caller (ImportItems) passes the DB's current status as the status parameter,
// so the DB status is preserved — import never changes status.
// Does NOT touch resolution/resolved_at — those are managed by Resolve().
func (m *Manager) UpdateFromImport(id int, severity, timeframe, scope, itemType, description, related string, position int, status string) error {
	_, err := m.store.Exec(`
		UPDATE backlog_items
		SET severity = ?, timeframe = ?, scope = ?, type = ?,
		    description = ?, related = ?, position = ?, status = ?,
		    updated_at = datetime('now','localtime')
		WHERE id = ?`,
		severity, timeframe, scope, itemType,
		description, related, position, status, id,
	)
	if err != nil {
		return fmt.Errorf("UpdateFromImport #%d: %w", id, err)
	}
	ml.Info("item updated from import", "id", id, "severity", severity, "timeframe", timeframe)
	return nil
}

// Resolve marks an item as resolved with the given resolution type.
// Returns an error if the item does not exist.
func (m *Manager) Resolve(id int, resolution string) error {
	if err := ValidateResolution(resolution); err != nil {
		return err
	}
	result, err := m.store.Exec(`
		UPDATE backlog_items
		SET status = ?,
		    resolution = ?,
		    resolved_at = datetime('now','localtime'),
		    updated_at = datetime('now','localtime')
		WHERE id = ?`,
		StatusResolved, resolution, id,
	)
	if err != nil {
		return fmt.Errorf("Resolve: %w", err)
	}
	n, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("Resolve rows affected: %w", err)
	}
	if n == 0 {
		return fmt.Errorf("Resolve: item %d not found", id)
	}
	ml.Info("item resolved", "id", id, "resolution", resolution)
	return nil
}

// SetStatus updates the status of a backlog item.
func (m *Manager) SetStatus(id int, status string) error {
	return m.SetStatusWith(m.store, id, status)
}

// SetStatusWith updates the status of a backlog item using the provided store
// (which may be a transaction-bound copy from RunInTx).
func (m *Manager) SetStatusWith(txs *store.Store, id int, status string) error {
	if err := ValidateStatus(status); err != nil {
		return err
	}
	result, err := txs.Exec(`
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

// Check returns whether a backlog item exists and its current status.
// Returns exists=false, status="", nil if the item does not exist.
func (m *Manager) Check(id int) (exists bool, status string, err error) {
	row := m.store.QueryRow("SELECT status FROM backlog_items WHERE id = ?", id)
	err = row.Scan(&status)
	if errors.Is(err, sql.ErrNoRows) {
		return false, "", nil
	}
	if err != nil {
		return false, "", fmt.Errorf("Check: %w", err)
	}
	return true, status, nil
}

// ListFixingForBranch returns backlog IDs from the given list that have status FIXING.
// Used by handoff merge gate to check for unresolved backlogs.
func (m *Manager) ListFixingForBranch(branch string, backlogIDs []int) ([]int, error) {
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

	rows, err := m.store.Query(query, args...)
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

