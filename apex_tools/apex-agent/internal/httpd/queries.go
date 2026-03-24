// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "fmt"

// ── Data Types ──
// httpd-local view types used by templates and JSON API.

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

// ── Query adapters — delegate to module querier interfaces ──

func queryDashboardSummary(bm BacklogQuerier, hm HandoffQuerier, qm QueueQuerier) (*DashboardSummary, error) {
	s := &DashboardSummary{}

	if bm != nil {
		statusCounts, err := bm.DashboardStatusCounts()
		if err != nil {
			return nil, fmt.Errorf("backlog status counts: %w", err)
		}
		s.BacklogOpen = statusCounts["OPEN"]
		s.BacklogFixing = statusCounts["FIXING"]
		s.BacklogResolved = statusCounts["RESOLVED"]

		sevCounts, err := bm.DashboardSeverityCounts()
		if err != nil {
			return nil, fmt.Errorf("backlog severity counts: %w", err)
		}
		s.CriticalCount = sevCounts["CRITICAL"]
		s.MajorCount = sevCounts["MAJOR"]
		s.MinorCount = sevCounts["MINOR"]
	}

	if hm != nil {
		count, err := hm.DashboardActiveCount()
		if err != nil {
			return nil, fmt.Errorf("active branches count: %w", err)
		}
		s.ActiveBranches = count
	}

	if qm != nil {
		buildLocked, err := qm.DashboardLockStatus("build")
		if err != nil {
			return nil, fmt.Errorf("build lock status: %w", err)
		}
		s.BuildLocked = buildLocked

		mergeLocked, err := qm.DashboardLockStatus("merge")
		if err != nil {
			return nil, fmt.Errorf("merge lock status: %w", err)
		}
		s.MergeLocked = mergeLocked
	}

	return s, nil
}

func queryBacklogList(bm BacklogQuerier, f BacklogFilter) ([]BacklogItem, error) {
	if bm == nil {
		return nil, nil
	}
	return bm.DashboardListItems(f)
}

func queryActiveBranches(hm HandoffQuerier) ([]ActiveBranch, error) {
	if hm == nil {
		return nil, nil
	}
	return hm.DashboardActiveBranchesList()
}

func queryBranchHistory(hm HandoffQuerier, limit int) ([]BranchHistory, error) {
	if hm == nil {
		return nil, nil
	}
	return hm.DashboardBranchHistoryList(limit)
}

func queryQueueStatus(qm QueueQuerier) ([]QueueEntry, error) {
	if qm == nil {
		return nil, nil
	}
	return qm.DashboardQueueAll()
}

// QueueHistoryEntry is an event log entry for queue state transitions.
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
