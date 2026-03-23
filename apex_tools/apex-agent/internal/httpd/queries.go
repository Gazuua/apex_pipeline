// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"fmt"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// ── Data Types ──

type DashboardSummary struct {
	BacklogOpen     int
	BacklogFixing   int
	BacklogResolved int
	CriticalCount   int
	MajorCount      int
	MinorCount      int
	ActiveBranches  int
	BuildLocked     bool
	MergeLocked     bool
}

type BacklogItem struct {
	ID          int
	Title       string
	Severity    string
	Timeframe   string
	Scope       string
	Type        string
	Status      string
	Description string
	Related     string
	Resolution  string
	CreatedAt   string
	UpdatedAt   string
}

type BacklogFilter struct {
	Status    []string
	Severity  []string
	Timeframe []string
	Scope     []string
	Type      []string
	SortBy    string
	SortDir   string
}

type ActiveBranch struct {
	Branch      string
	WorkspaceID string
	GitBranch   string
	Summary     string
	Status      string
	BacklogIDs  []int
	CreatedAt   string
}

type BranchHistory struct {
	ID         int
	Branch     string
	GitBranch  string
	Summary    string
	Status     string
	BacklogIDs string
	StartedAt  string
	FinishedAt string
}

type QueueEntry struct {
	Channel    string
	Branch     string
	Status     string
	CreatedAt  string
	FinishedAt string
	Duration   string // computed: finished_at - created_at
}

// ── Queries ──

func queryDashboardSummary(st *store.Store) (*DashboardSummary, error) {
	s := &DashboardSummary{}

	// Status counts
	rows, err := st.Query(`SELECT status, COUNT(*) FROM backlog_items GROUP BY status`)
	if err != nil {
		return nil, fmt.Errorf("backlog status counts: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var status string
		var count int
		if err := rows.Scan(&status, &count); err != nil {
			return nil, err
		}
		switch status {
		case "OPEN":
			s.BacklogOpen = count
		case "FIXING":
			s.BacklogFixing = count
		case "RESOLVED":
			s.BacklogResolved = count
		}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("backlog status counts: rows: %w", err)
	}

	// Severity counts (non-resolved only)
	rows2, err := st.Query(`SELECT severity, COUNT(*) FROM backlog_items WHERE status != 'RESOLVED' GROUP BY severity`)
	if err != nil {
		return nil, fmt.Errorf("backlog severity counts: %w", err)
	}
	defer rows2.Close()
	for rows2.Next() {
		var sev string
		var count int
		if err := rows2.Scan(&sev, &count); err != nil {
			return nil, err
		}
		switch sev {
		case "CRITICAL":
			s.CriticalCount = count
		case "MAJOR":
			s.MajorCount = count
		case "MINOR":
			s.MinorCount = count
		}
	}
	if err := rows2.Err(); err != nil {
		return nil, fmt.Errorf("backlog severity counts: rows: %w", err)
	}

	// Active branches count
	if err := st.QueryRow(`SELECT COUNT(*) FROM active_branches`).Scan(&s.ActiveBranches); err != nil {
		return nil, fmt.Errorf("active branches count: %w", err)
	}

	// Queue lock status
	var buildHolding int
	if err := st.QueryRow(`SELECT COUNT(*) FROM queue WHERE channel='build' AND status='ACTIVE'`).Scan(&buildHolding); err != nil {
		return nil, fmt.Errorf("build lock status: %w", err)
	}
	s.BuildLocked = buildHolding > 0

	var mergeHolding int
	if err := st.QueryRow(`SELECT COUNT(*) FROM queue WHERE channel='merge' AND status='ACTIVE'`).Scan(&mergeHolding); err != nil {
		return nil, fmt.Errorf("merge lock status: %w", err)
	}
	s.MergeLocked = mergeHolding > 0

	return s, nil
}

func queryBacklogList(st *store.Store, f BacklogFilter) ([]BacklogItem, error) {
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
	addInFilter("scope", f.Scope)
	addInFilter("type", f.Type)

	query := `SELECT id, title, severity, timeframe, scope, type, status, description,
	          COALESCE(related,''), COALESCE(resolution,''), created_at, updated_at
	          FROM backlog_items`
	if len(where) > 0 {
		query += " WHERE " + strings.Join(where, " AND ")
	}

	// Sort — default: FIXING first → timeframe urgency → severity → ID
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
		// Default smart sort:
		// 1. FIXING first (actively being worked on)
		// 2. Timeframe urgency: NOW > IN_VIEW > DEFERRED > others
		// 3. Severity: CRITICAL > MAJOR > MINOR
		// 4. ID descending (newest first)
		query += ` ORDER BY
			CASE status WHEN 'FIXING' THEN 0 WHEN 'OPEN' THEN 1 ELSE 2 END,
			CASE timeframe WHEN 'NOW' THEN 0 WHEN 'IN_VIEW' THEN 1 WHEN 'DEFERRED' THEN 2 ELSE 3 END,
			CASE severity WHEN 'CRITICAL' THEN 0 WHEN 'MAJOR' THEN 1 WHEN 'MINOR' THEN 2 ELSE 3 END,
			id DESC`
	}

	rows, err := st.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("backlog list: %w", err)
	}
	defer rows.Close()

	var items []BacklogItem
	for rows.Next() {
		var b BacklogItem
		if err := rows.Scan(&b.ID, &b.Title, &b.Severity, &b.Timeframe, &b.Scope, &b.Type,
			&b.Status, &b.Description, &b.Related, &b.Resolution, &b.CreatedAt, &b.UpdatedAt); err != nil {
			return nil, err
		}
		items = append(items, b)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("backlog list: rows: %w", err)
	}
	return items, nil
}

func queryActiveBranches(st *store.Store) ([]ActiveBranch, error) {
	rows, err := st.Query(`
		SELECT branch, workspace, COALESCE(git_branch,''), COALESCE(summary,''),
		       status, created_at
		FROM active_branches
		ORDER BY created_at DESC`)
	if err != nil {
		return nil, fmt.Errorf("active branches: %w", err)
	}

	// Collect all branches first, then close rows before nested queries.
	// SQLite :memory: with MaxOpenConns(1) deadlocks on nested queries.
	var branches []ActiveBranch
	for rows.Next() {
		var ab ActiveBranch
		if err := rows.Scan(&ab.Branch, &ab.WorkspaceID, &ab.GitBranch, &ab.Summary,
			&ab.Status, &ab.CreatedAt); err != nil {
			rows.Close()
			return nil, err
		}
		branches = append(branches, ab)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, fmt.Errorf("active branches: rows: %w", err)
	}
	rows.Close()

	// Fetch linked backlog IDs per branch
	for i := range branches {
		bbRows, err := st.Query(`SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branches[i].Branch)
		if err != nil {
			continue
		}
		for bbRows.Next() {
			var bid int
			bbRows.Scan(&bid) //nolint:errcheck
			branches[i].BacklogIDs = append(branches[i].BacklogIDs, bid)
		}
		bbRows.Close()
	}

	return branches, nil
}

func queryBranchHistory(st *store.Store, limit int) ([]BranchHistory, error) {
	rows, err := st.Query(`
		SELECT id, branch, COALESCE(git_branch,''), COALESCE(summary,''),
		       status, COALESCE(backlog_ids,''), started_at, finished_at
		FROM branch_history
		ORDER BY finished_at DESC
		LIMIT ?`, limit)
	if err != nil {
		return nil, fmt.Errorf("branch history: %w", err)
	}
	defer rows.Close()

	var history []BranchHistory
	for rows.Next() {
		var bh BranchHistory
		if err := rows.Scan(&bh.ID, &bh.Branch, &bh.GitBranch, &bh.Summary,
			&bh.Status, &bh.BacklogIDs, &bh.StartedAt, &bh.FinishedAt); err != nil {
			return nil, err
		}
		history = append(history, bh)
	}
	return history, nil
}

func queryQueueStatus(st *store.Store) ([]QueueEntry, error) {
	rows, err := st.Query(`
		SELECT channel, branch, status, created_at, COALESCE(finished_at,''),
		       CASE WHEN finished_at IS NOT NULL
		            THEN CAST((julianday(finished_at) - julianday(created_at)) * 86400 AS INTEGER)
		            ELSE 0 END as duration_sec
		FROM queue
		ORDER BY channel, created_at DESC`)
	if err != nil {
		return nil, fmt.Errorf("queue status: %w", err)
	}
	defer rows.Close()

	var entries []QueueEntry
	for rows.Next() {
		var q QueueEntry
		var durSec int
		if err := rows.Scan(&q.Channel, &q.Branch, &q.Status, &q.CreatedAt, &q.FinishedAt, &durSec); err != nil {
			return nil, err
		}
		if durSec > 0 {
			h := durSec / 3600
			m := (durSec % 3600) / 60
			s := durSec % 60
			if h > 0 {
				q.Duration = fmt.Sprintf("%dh %dm %ds", h, m, s)
			} else if m > 0 {
				q.Duration = fmt.Sprintf("%dm %ds", m, s)
			} else {
				q.Duration = fmt.Sprintf("%ds", s)
			}
		}
		entries = append(entries, q)
	}
	return entries, nil
}
