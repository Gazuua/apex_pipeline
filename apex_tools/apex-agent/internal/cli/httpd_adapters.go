// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/httpd"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/handoff"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/queue"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/workspace"
)

// ── BacklogQuerier adapter ──

type backlogQuerierAdapter struct {
	mgr *backlog.Manager
}

func (a *backlogQuerierAdapter) DashboardStatusCounts() (map[string]int, error) {
	return a.mgr.DashboardStatusCounts(context.Background())
}

func (a *backlogQuerierAdapter) DashboardSeverityCounts() (map[string]int, error) {
	return a.mgr.DashboardSeverityCounts(context.Background())
}

func (a *backlogQuerierAdapter) DashboardListItems(f httpd.BacklogFilter) ([]httpd.BacklogItem, error) {
	mf := backlog.DashboardFilter{
		Status:    f.Status,
		Severity:  f.Severity,
		Timeframe: f.Timeframe,
		Scope:     f.Scope,
		Type:      f.Type,
		SortBy:    f.SortBy,
		SortDir:   f.SortDir,
	}
	items, err := a.mgr.DashboardList(context.Background(), mf)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.BacklogItem, len(items))
	for i, item := range items {
		result[i] = httpd.BacklogItem{
			ID:          item.ID,
			Title:       item.Title,
			Severity:    item.Severity,
			Timeframe:   item.Timeframe,
			Scope:       item.Scope,
			Type:        item.Type,
			Status:      item.Status,
			Description: item.Description,
			Related:     item.Related,
			Resolution:  item.Resolution,
			CreatedAt:   item.CreatedAt,
			UpdatedAt:   item.UpdatedAt,
		}
	}
	return result, nil
}

func (a *backlogQuerierAdapter) DashboardGetItemByID(id int) (*httpd.BacklogItem, error) {
	item, err := a.mgr.DashboardGetByID(context.Background(), id)
	if err != nil {
		return nil, err
	}
	if item == nil {
		return nil, nil
	}
	return &httpd.BacklogItem{
		ID:          item.ID,
		Title:       item.Title,
		Severity:    item.Severity,
		Timeframe:   item.Timeframe,
		Scope:       item.Scope,
		Type:        item.Type,
		Status:      item.Status,
		Description: item.Description,
		Related:     item.Related,
		Resolution:  item.Resolution,
		CreatedAt:   item.CreatedAt,
		UpdatedAt:   item.UpdatedAt,
	}, nil
}

// ── HandoffQuerier adapter ──

type handoffQuerierAdapter struct {
	mgr *handoff.Manager
}

func (a *handoffQuerierAdapter) DashboardActiveBranchesList() ([]httpd.ActiveBranch, error) {
	branches, err := a.mgr.DashboardActiveBranches(context.Background())
	if err != nil {
		return nil, err
	}
	result := make([]httpd.ActiveBranch, len(branches))
	for i, b := range branches {
		result[i] = httpd.ActiveBranch{
			Branch:      b.Branch,
			WorkspaceID: b.WorkspaceID,
			GitBranch:   b.GitBranch,
			Summary:     b.Summary,
			Status:      b.Status,
			BacklogIDs:  b.BacklogIDs,
			CreatedAt:   b.CreatedAt,
		}
	}
	return result, nil
}

func (a *handoffQuerierAdapter) DashboardActiveCount() (int, error) {
	return a.mgr.DashboardActiveCount(context.Background())
}

func (a *handoffQuerierAdapter) DashboardBranchHistoryList(limit int) ([]httpd.BranchHistory, error) {
	history, err := a.mgr.DashboardBranchHistoryList(context.Background(), limit)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.BranchHistory, len(history))
	for i, h := range history {
		result[i] = httpd.BranchHistory{
			ID:         h.ID,
			Branch:     h.Branch,
			GitBranch:  h.GitBranch,
			Summary:    h.Summary,
			Status:     h.Status,
			BacklogIDs: h.BacklogIDs,
			StartedAt:  h.StartedAt,
			FinishedAt: h.FinishedAt,
		}
	}
	return result, nil
}

// ── QueueQuerier adapter ──

type queueQuerierAdapter struct {
	mgr *queue.Manager
}

func (a *queueQuerierAdapter) DashboardQueueAll() ([]httpd.QueueEntry, error) {
	entries, err := a.mgr.DashboardQueueAll(context.Background())
	if err != nil {
		return nil, err
	}
	result := make([]httpd.QueueEntry, len(entries))
	for i, e := range entries {
		result[i] = httpd.QueueEntry{
			Channel:    e.Channel,
			Branch:     e.Branch,
			Status:     e.Status,
			CreatedAt:  e.CreatedAt,
			FinishedAt: e.FinishedAt,
		}
		if e.DurationSec > 0 {
			h := e.DurationSec / 3600
			m := (e.DurationSec % 3600) / 60
			s := e.DurationSec % 60
			if h > 0 {
				result[i].Duration = fmt.Sprintf("%dh %dm %ds", h, m, s)
			} else if m > 0 {
				result[i].Duration = fmt.Sprintf("%dm %ds", m, s)
			} else {
				result[i].Duration = fmt.Sprintf("%ds", s)
			}
		}
	}
	return result, nil
}

func (a *queueQuerierAdapter) DashboardLockStatus(channel string) (bool, error) {
	return a.mgr.DashboardLockStatus(context.Background(), channel)
}

func (a *queueQuerierAdapter) DashboardQueueHistory(channel string, offset, limit int, from, to string) ([]httpd.QueueHistoryEntry, error) {
	entries, err := a.mgr.DashboardHistory(context.Background(), channel, offset, limit, from, to)
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

// ── WorkspaceQuerier adapter ──

type workspaceQuerierAdapter struct {
	mgr *workspace.Manager
}

func (a *workspaceQuerierAdapter) DashboardBranchesList() ([]httpd.BranchInfo, error) {
	ctx := context.Background()
	branches, err := a.mgr.DashboardBranchesList(ctx)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.BranchInfo, len(branches))
	for i, b := range branches {
		result[i] = httpd.BranchInfo{
			WorkspaceID:   b.WorkspaceID,
			Directory:     b.Directory,
			GitBranch:     b.GitBranch,
			GitStatus:     b.GitStatus,
			SessionStatus: b.SessionStatus,
			SessionID:     b.SessionID,
			HandoffStatus: b.HandoffStatus,
			BacklogIDs:    b.BacklogIDs,
		}
		// Fetch blocked backlogs for branches that have backlog IDs.
		if b.BacklogIDs != "" {
			blocked, _ := a.mgr.DashboardBlockedBacklogs(ctx, b.BacklogIDs)
			for _, bb := range blocked {
				result[i].BlockedBacklogs = append(result[i].BlockedBacklogs, httpd.BlockedBacklogInfo{
					ID:            bb.ID,
					Title:         bb.Title,
					BlockedReason: bb.BlockedReason,
				})
			}
		}
	}
	return result, nil
}

func (a *workspaceQuerierAdapter) DashboardBlockedCount() (int, error) {
	return a.mgr.DashboardBlockedCount(context.Background())
}
