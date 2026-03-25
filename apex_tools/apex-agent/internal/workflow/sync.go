// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"context"
	"os"
	"path/filepath"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
)

// BacklogSyncer abstracts backlog import/export for the merge pipeline.
// Implemented by *backlog.Manager. Using an interface decouples workflow
// from the concrete Manager, enabling easier testing and preventing
// the handoff module from depending on the full backlog package.
type BacklogSyncer interface {
	// SafeExportJSON runs import-first (restore from files) then exports DB → JSON.
	// Returns (jsonBytes, importCount, error).
	SafeExportJSON(ctx context.Context, jsonData []byte, legacyMD, legacyHistory string) ([]byte, int, error)

	// ImportItems inserts or updates parsed items in the database.
	// New items are inserted; existing items have metadata updated (status is never overwritten).
	ImportItems(ctx context.Context, items []backlog.BacklogItem) (int, error)
}

// SyncImport reads BACKLOG.json (or legacy MD files) and imports into DB.
// Prefers BACKLOG.json if it exists; falls back to BACKLOG.md + BACKLOG_HISTORY.md.
// Idempotent — existing items keep their status, only metadata is updated.
// Returns the number of items processed. Missing files are not an error.
func SyncImport(ctx context.Context, projectRoot string, syncer BacklogSyncer) (int, error) {
	total := 0

	// Prefer JSON format
	jsonPath := filepath.Join(projectRoot, "docs", "BACKLOG.json")
	if data, err := os.ReadFile(jsonPath); err == nil {
		items, parseErr := backlog.ParseBacklogJSON(data)
		if parseErr != nil {
			return total, parseErr
		}
		n, importErr := syncer.ImportItems(ctx, items)
		if importErr != nil {
			return total, importErr
		}
		total += n

		if total > 0 {
			ml.Info("backlog import 완료", "items", total)
		}
		return total, nil
	}

	// Fallback: legacy MD files
	backlogPath := filepath.Join(projectRoot, "docs", "BACKLOG.md")
	if data, err := os.ReadFile(backlogPath); err == nil {
		items, parseErr := backlog.ParseBacklogMD(string(data))
		if parseErr != nil {
			return total, parseErr
		}
		n, importErr := syncer.ImportItems(ctx, items)
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
		n, importErr := syncer.ImportItems(ctx, items)
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

// SyncExport runs SafeExportJSON (import-first + export in single TX) and
// writes the result to docs/BACKLOG.json.
// Also handles migration: if legacy MD files exist, imports them and removes them.
// Returns the number of items synced during import-first phase.
func SyncExport(ctx context.Context, projectRoot string, syncer BacklogSyncer) (int, error) {
	jsonPath := filepath.Join(projectRoot, "docs", "BACKLOG.json")
	backlogMDPath := filepath.Join(projectRoot, "docs", "BACKLOG.md")
	historyMDPath := filepath.Join(projectRoot, "docs", "BACKLOG_HISTORY.md")

	// Read existing files for import-first
	jsonData, _ := os.ReadFile(jsonPath)
	legacyBacklogMD := ""
	legacyHistoryMD := ""
	if data, err := os.ReadFile(backlogMDPath); err == nil {
		legacyBacklogMD = string(data)
	}
	if data, err := os.ReadFile(historyMDPath); err == nil {
		legacyHistoryMD = string(data)
	}

	out, imported, err := syncer.SafeExportJSON(ctx, jsonData, legacyBacklogMD, legacyHistoryMD)
	if err != nil {
		return imported, err
	}

	if err := os.MkdirAll(filepath.Join(projectRoot, "docs"), 0o755); err != nil {
		return imported, err
	}
	if err := os.WriteFile(jsonPath, out, 0o644); err != nil {
		return imported, err
	}

	// Migration: remove legacy MD files if they exist and JSON was written
	if legacyBacklogMD != "" {
		if rmErr := os.Remove(backlogMDPath); rmErr == nil {
			ml.Info("레거시 BACKLOG.md 삭제 완료 (JSON 마이그레이션)")
		}
	}
	if legacyHistoryMD != "" {
		if rmErr := os.Remove(historyMDPath); rmErr == nil {
			ml.Info("레거시 BACKLOG_HISTORY.md 삭제 완료 (JSON 마이그레이션)")
		}
	}

	ml.Info("backlog export 완료 (JSON)", "imported", imported)
	return imported, nil
}
