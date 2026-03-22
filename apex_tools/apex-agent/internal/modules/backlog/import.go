// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"bufio"
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

var itemHeaderRe = regexp.MustCompile(`^###\s+#(\d+)\.\s+(.+)$`)
var fieldRe = regexp.MustCompile(`^-\s+\*\*(.+?)\*\*:\s*(.+)$`)
var inlineFieldRe = regexp.MustCompile(`\*\*(.+?)\*\*:\s*(.+)`)

// ParseBacklogMD parses a BACKLOG.md file content into BacklogItems.
func ParseBacklogMD(content string) ([]BacklogItem, error) {
	var items []BacklogItem
	var current *BacklogItem
	currentTimeframe := ""
	position := 0

	scanner := bufio.NewScanner(strings.NewReader(content))
	for scanner.Scan() {
		line := scanner.Text()

		// Detect timeframe section
		if strings.HasPrefix(line, "## NOW") {
			currentTimeframe = "NOW"
			position = 0
			continue
		}
		if strings.HasPrefix(line, "## IN VIEW") {
			currentTimeframe = "IN_VIEW"
			position = 0
			continue
		}
		if strings.HasPrefix(line, "## DEFERRED") {
			currentTimeframe = "DEFERRED"
			position = 0
			continue
		}

		// Detect item header: ### #N. Title
		if m := itemHeaderRe.FindStringSubmatch(line); m != nil {
			if current != nil {
				items = append(items, *current)
			}
			id, _ := strconv.Atoi(m[1])
			position++
			current = &BacklogItem{
				ID:        id,
				Title:     m[2],
				Timeframe: currentTimeframe,
				Position:  position,
				Status:    StatusOpen,
			}
			continue
		}

		// Detect field: - **Key**: Value
		if current != nil {
			if m := fieldRe.FindStringSubmatch(line); m != nil {
				key, value := m[1], m[2]
				switch key {
				case "등급":
					current.Severity = strings.ToUpper(strings.TrimSpace(value))
				case "스코프":
					current.Scope = NormalizeScope(value)
				case "타입":
					current.Type = NormalizeType(value)
				case "연관":
					// "#50, #89" → "50,89"
					value = strings.ReplaceAll(value, "#", "")
					value = strings.ReplaceAll(value, " ", "")
					current.Related = value
				case "설명":
					current.Description = value
				}
			} else if current.Description != "" && !strings.HasPrefix(line, "###") && !strings.HasPrefix(line, "- **") && strings.TrimSpace(line) != "" {
				// 멀티라인 설명 연속 — 이전 줄에 설명이 시작됐고, 새 필드/항목이 아닌 줄
				current.Description += " " + strings.TrimSpace(line)
			}
		}
	}

	// Don't forget the last item
	if current != nil {
		items = append(items, *current)
	}

	return items, nil
}

// ParseBacklogHistoryMD parses BACKLOG_HISTORY.md into resolved BacklogItems.
func ParseBacklogHistoryMD(content string) ([]BacklogItem, error) {
	var items []BacklogItem
	var current *BacklogItem

	scanner := bufio.NewScanner(strings.NewReader(content))
	for scanner.Scan() {
		line := scanner.Text()

		// Item header: ### #N. Title
		if m := itemHeaderRe.FindStringSubmatch(line); m != nil {
			if current != nil {
				items = append(items, *current)
			}
			id, _ := strconv.Atoi(m[1])
			current = &BacklogItem{
				ID:     id,
				Title:  m[2],
				Status: StatusResolved,
			}
			continue
		}

		if current == nil {
			continue
		}

		// History format: - **등급**: MAJOR | **스코프**: auth-svc | **타입**: bug
		// Parse pipe-separated fields on one line
		if strings.HasPrefix(line, "- **등급**:") {
			parts := strings.Split(line, "|")
			for _, part := range parts {
				part = strings.TrimSpace(part)
				if m := inlineFieldRe.FindStringSubmatch(part); m != nil {
					switch m[1] {
					case "등급":
						current.Severity = strings.ToUpper(strings.TrimSpace(m[2]))
					case "스코프":
						current.Scope = NormalizeScope(m[2])
					case "타입":
						current.Type = NormalizeType(strings.TrimSpace(m[2]))
					}
				}
			}
		}

		// Resolution line: - **해결**: DATE | **방식**: FIXED | **커밋**: hash
		if strings.HasPrefix(line, "- **해결**:") {
			parts := strings.Split(line, "|")
			for _, part := range parts {
				part = strings.TrimSpace(part)
				if m := inlineFieldRe.FindStringSubmatch(part); m != nil {
					switch m[1] {
					case "해결":
						current.ResolvedAt = strings.TrimSpace(m[2])
					case "방식":
						current.Resolution = NormalizeResolution(m[2])
					}
				}
			}
		}

		// Description line: - **비고**: text
		if strings.HasPrefix(line, "- **비고**:") {
			if m := fieldRe.FindStringSubmatch(line); m != nil {
				current.Description = m[2]
			}
		}
	}

	if current != nil {
		items = append(items, *current)
	}

	return items, nil
}

// ImportItems inserts parsed items into the database.
func (mgr *Manager) ImportItems(items []BacklogItem) (int, error) {
	count := 0
	for _, item := range items {
		// Skip if ID already exists
		exists, _, err := mgr.Check(item.ID)
		if err != nil {
			return count, err
		}
		if exists {
			continue
		}

		if err := mgr.Add(&item); err != nil {
			return count, fmt.Errorf("import #%d: %w", item.ID, err)
		}

		// If item was resolved, mark it as such
		if item.Status == StatusResolved && item.Resolution != "" {
			if err := mgr.Resolve(item.ID, item.Resolution); err != nil {
				return count, fmt.Errorf("resolve #%d: %w", item.ID, err)
			}
		}
		count++
	}
	return count, nil
}
