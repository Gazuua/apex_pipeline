// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"database/sql"
	"errors"
	"fmt"

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
	Status      string // open, resolved
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
		status = "open"
	}

	_, err := m.store.Exec(`
		INSERT INTO backlog_items
			(id, title, severity, timeframe, scope, type, description, related,
			 position, status, resolution, resolved_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		item.ID, item.Title, item.Severity, item.Timeframe, item.Scope, item.Type,
		item.Description, item.Related, item.Position, status,
		nullableString(item.Resolution), nullableString(item.ResolvedAt),
	)
	if err != nil {
		return fmt.Errorf("Add: %w", err)
	}
	ml.Info("item added", "id", item.ID, "severity", item.Severity, "timeframe", item.Timeframe)
	return nil
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

	item := &BacklogItem{}
	err := row.Scan(
		&item.ID, &item.Title, &item.Severity, &item.Timeframe, &item.Scope, &item.Type,
		&item.Description, &item.Related, &item.Position, &item.Status,
		&item.Resolution, &item.ResolvedAt, &item.CreatedAt, &item.UpdatedAt,
	)
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
// If filter.Status is empty, defaults to "open".
func (m *Manager) List(filter ListFilter) ([]BacklogItem, error) {
	status := filter.Status
	if status == "" {
		status = "open"
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
		var item BacklogItem
		if err := rows.Scan(
			&item.ID, &item.Title, &item.Severity, &item.Timeframe, &item.Scope, &item.Type,
			&item.Description, &item.Related, &item.Position, &item.Status,
			&item.Resolution, &item.ResolvedAt, &item.CreatedAt, &item.UpdatedAt,
		); err != nil {
			return nil, fmt.Errorf("List scan: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("List rows: %w", err)
	}
	return items, nil
}

// Resolve marks an item as resolved with the given resolution type.
// Returns an error if the item does not exist.
func (m *Manager) Resolve(id int, resolution string) error {
	result, err := m.store.Exec(`
		UPDATE backlog_items
		SET status = 'resolved',
		    resolution = ?,
		    resolved_at = datetime('now','localtime'),
		    updated_at = datetime('now','localtime')
		WHERE id = ?`,
		resolution, id,
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

// nullableString converts an empty string to nil for SQL NULL storage.
func nullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}
