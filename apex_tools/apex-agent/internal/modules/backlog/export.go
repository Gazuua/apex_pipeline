// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// SafeExport runs Import first (MD → DB) to ensure DB is a superset of the
// current MD, then exports. This prevents data loss if the local DB was
// deleted or corrupted — Import restores from MD before Export overwrites it.
// Import + Export are wrapped in a single transaction for atomicity.
//
// Returns (content, importCount, error).
func (mgr *Manager) SafeExport(backlogMD, historyMD string) (string, int, error) {
	var content string
	var imported int

	err := mgr.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
		txMgr := mgr.withQuerier(tx)

		// 1) Import current MD into DB (idempotent — existing items keep their status).
		if backlogMD != "" {
			items, err := ParseBacklogMD(backlogMD)
			if err != nil {
				return fmt.Errorf("parse BACKLOG.md: %w", err)
			}
			n, err := txMgr.ImportItems(items)
			if err != nil {
				return fmt.Errorf("import BACKLOG.md: %w", err)
			}
			imported += n
		}
		if historyMD != "" {
			items, err := ParseBacklogHistoryMD(historyMD)
			if err != nil {
				return fmt.Errorf("parse BACKLOG_HISTORY.md: %w", err)
			}
			n, err := txMgr.ImportItems(items)
			if err != nil {
				return fmt.Errorf("import BACKLOG_HISTORY.md: %w", err)
			}
			imported += n
		}

		// 2) Export from DB.
		var exportErr error
		content, exportErr = txMgr.Export()
		return exportErr
	})
	if err != nil {
		return "", imported, err
	}

	return content, imported, nil
}

// Export generates BACKLOG.md content from the database.
func (mgr *Manager) Export() (string, error) {
	nextID, err := mgr.NextID()
	if err != nil {
		return "", fmt.Errorf("next id: %w", err)
	}

	var b strings.Builder

	// Header
	b.WriteString("# BACKLOG\n\n")
	b.WriteString("미해결 이슈 집약. 새 작업 시작 전 반드시 확인.\n")
	b.WriteString("완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.\n")
	b.WriteString("운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.\n\n")
	fmt.Fprintf(&b, "다음 발번: %d\n\n", nextID)
	b.WriteString("---\n\n")

	// Sections
	sections := []struct {
		dbValue   string
		mdHeading string
	}{
		{"NOW", "NOW"},
		{"IN_VIEW", "IN VIEW"},
		{"DEFERRED", "DEFERRED"},
	}

	for i, sec := range sections {
		// OPEN + FIXING 모두 출력 — FIXING은 작업 중이지만 아직 미해결
		openItems, err := mgr.List(ListFilter{Timeframe: sec.dbValue, Status: StatusOpen})
		if err != nil {
			return "", fmt.Errorf("list %s open: %w", sec.dbValue, err)
		}
		fixingItems, err := mgr.List(ListFilter{Timeframe: sec.dbValue, Status: StatusFixing})
		if err != nil {
			return "", fmt.Errorf("list %s fixing: %w", sec.dbValue, err)
		}
		items := append(openItems, fixingItems...)

		fmt.Fprintf(&b, "## %s\n", sec.mdHeading)

		if len(items) > 0 {
			b.WriteString("\n")
			for _, item := range items {
				writeItem(&b, &item)
			}
		}

		if i < len(sections)-1 {
			b.WriteString("\n---\n\n")
		} else {
			b.WriteString("\n")
		}
	}

	return b.String(), nil
}

// ExportHistory generates updated BACKLOG_HISTORY.md content.
// New RESOLVED items (not already in existingHistory) are prepended
// after the <!-- NEW_ENTRY_BELOW --> marker.
func (mgr *Manager) ExportHistory(existingHistory string) (string, error) {
	resolved, err := mgr.List(ListFilter{Status: StatusResolved})
	if err != nil {
		return existingHistory, fmt.Errorf("list resolved: %w", err)
	}
	if len(resolved) == 0 {
		return existingHistory, nil
	}

	existingIDs := parseHistoryIDs(existingHistory)

	var newItems []BacklogItem
	for _, item := range resolved {
		if !existingIDs[item.ID] {
			newItems = append(newItems, item)
		}
	}
	if len(newItems) == 0 {
		return existingHistory, nil
	}

	var insertion strings.Builder
	for _, item := range newItems {
		writeHistoryItem(&insertion, &item)
	}

	const marker = "<!-- NEW_ENTRY_BELOW -->"
	idx := strings.Index(existingHistory, marker)
	if idx < 0 {
		header := "# BACKLOG HISTORY\n\n"
		if strings.HasPrefix(existingHistory, header) {
			return header + marker + "\n\n" + insertion.String() + existingHistory[len(header):], nil
		}
		return existingHistory + "\n" + marker + "\n\n" + insertion.String(), nil
	}

	insertPos := idx + len(marker) + 1
	return existingHistory[:insertPos] + "\n" + insertion.String() + existingHistory[insertPos:], nil
}

func parseHistoryIDs(content string) map[int]bool {
	ids := make(map[int]bool)
	for _, line := range strings.Split(content, "\n") {
		if m := itemHeaderRe.FindStringSubmatch(line); m != nil {
			id, _ := strconv.Atoi(m[1])
			ids[id] = true
		}
	}
	return ids
}

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

func writeItem(b *strings.Builder, item *BacklogItem) {
	fmt.Fprintf(b, "### #%d. %s\n", item.ID, item.Title)
	fmt.Fprintf(b, "- **등급**: %s\n", item.Severity)
	fmt.Fprintf(b, "- **스코프**: %s\n", item.Scope)
	fmt.Fprintf(b, "- **타입**: %s\n", item.Type)

	if item.Related != "" {
		// Convert "50,89" → "#50, #89"
		parts := strings.Split(item.Related, ",")
		var refs []string
		for _, p := range parts {
			p = strings.TrimSpace(p)
			if p != "" {
				refs = append(refs, "#"+p)
			}
		}
		fmt.Fprintf(b, "- **연관**: %s\n", strings.Join(refs, ", "))
	}

	fmt.Fprintf(b, "- **설명**: %s\n", item.Description)
	b.WriteString("\n")
}
