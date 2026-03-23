// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
)

// SyncImport reads BACKLOG.md + BACKLOG_HISTORY.md and imports into DB.
// Idempotent — existing items keep their status, only metadata is updated.
// Returns the number of items processed. Missing files are not an error.
func SyncImport(projectRoot string, mgr *backlog.Manager) (int, error) {
	total := 0

	backlogPath := filepath.Join(projectRoot, "docs", "BACKLOG.md")
	if data, err := os.ReadFile(backlogPath); err == nil {
		items, parseErr := backlog.ParseBacklogMD(string(data))
		if parseErr != nil {
			return total, parseErr
		}
		n, importErr := mgr.ImportItems(items)
		if importErr != nil {
			return total, importErr
		}
		total += n
	}

	historyPath := filepath.Join(projectRoot, "docs", "BACKLOG_HISTORY.md")
	if data, err := os.ReadFile(historyPath); err == nil {
		items, parseErr := backlog.ParseBacklogHistoryMD(string(data))
		if parseErr != nil {
			return total, parseErr
		}
		n, importErr := mgr.ImportItems(items)
		if importErr != nil {
			return total, importErr
		}
		total += n
	}

	if total > 0 {
		ml.Info("backlog import 완료", "items", total)
	}
	return total, nil
}

// SyncExport runs SafeExport (import-first + export in single TX) and
// writes the result to docs/BACKLOG.md.
// Returns the number of items synced during import-first phase.
func SyncExport(projectRoot string, mgr *backlog.Manager) (int, error) {
	backlogPath := filepath.Join(projectRoot, "docs", "BACKLOG.md")
	historyPath := filepath.Join(projectRoot, "docs", "BACKLOG_HISTORY.md")

	backlogData, _ := os.ReadFile(backlogPath)
	historyData, _ := os.ReadFile(historyPath)

	content, imported, err := mgr.SafeExport(string(backlogData), string(historyData))
	if err != nil {
		return imported, err
	}

	if err := os.MkdirAll(filepath.Join(projectRoot, "docs"), 0o755); err != nil {
		return imported, err
	}
	if err := os.WriteFile(backlogPath, []byte(content), 0o644); err != nil {
		return imported, err
	}

	// HISTORY 쓰기 — RESOLVED 항목 자동 이관
	// SafeExport는 history를 import만 하므로 파일 내용은 변경되지 않음 → historyData 재사용
	historyStr := string(historyData)
	updatedHistory, err := mgr.ExportHistory(historyStr)
	if err != nil {
		return imported, fmt.Errorf("export history: %w", err)
	}
	if updatedHistory != historyStr {
		if err := os.WriteFile(historyPath, []byte(updatedHistory), 0o644); err != nil {
			return imported, fmt.Errorf("write history: %w", err)
		}
		ml.Info("backlog history 갱신 완료")
	}

	ml.Info("backlog export 완료", "imported", imported)
	return imported, nil
}
