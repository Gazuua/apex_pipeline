// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"fmt"
	"strings"
)

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
		items, err := mgr.List(ListFilter{Timeframe: sec.dbValue, Status: "open"})
		if err != nil {
			return "", fmt.Errorf("list %s: %w", sec.dbValue, err)
		}

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
