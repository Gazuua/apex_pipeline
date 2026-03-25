// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// ── Params types ──

type addParams struct {
	BacklogItem
	Fix    bool   `json:"fix"`
	Branch string `json:"branch"`
}

type getParams struct {
	ID int `json:"id"`
}

type resolveParams struct {
	ID         int    `json:"id"`
	Resolution string `json:"resolution"`
}

type checkParams struct {
	ID int `json:"id"`
}

type updateParams struct {
	ID     int               `json:"id"`
	Fields map[string]string `json:"fields"`
}

type releaseParams struct {
	ID     int    `json:"id"`
	Reason string `json:"reason"`
	Branch string `json:"branch"`
}

type fixParams struct {
	ID     int    `json:"id"`
	Branch string `json:"branch"`
}

type syncImportParams struct {
	JSONData string `json:"json_data"`
}

// Module implements the daemon.Module interface for backlog management.
type Module struct {
	manager *Manager
}

// New creates a new backlog Module backed by the given store.
func New(s *store.Store) *Module {
	return &Module{manager: NewManager(s)}
}

func (m *Module) Name() string { return "backlog" }

// Manager returns the underlying Manager for cross-module use.
func (m *Module) Manager() *Manager { return m.manager }

// RegisterSchema registers the backlog_items table migration.
func (m *Module) RegisterSchema(mig *store.Migrator) {
	mig.Register("backlog", 1, func(tx *store.TxStore) error {
		ctx := context.Background()
		_, err := tx.Exec(ctx, `CREATE TABLE backlog_items (
			id          INTEGER PRIMARY KEY,
			title       TEXT    NOT NULL,
			severity    TEXT    NOT NULL,
			timeframe   TEXT    NOT NULL,
			scope       TEXT    NOT NULL,
			type        TEXT    NOT NULL,
			description TEXT    NOT NULL,
			related     TEXT,
			position    INTEGER NOT NULL,
			status      TEXT    NOT NULL DEFAULT 'open',
			resolution  TEXT,
			resolved_at TEXT,
			created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
		)`)
		return err
	})
	mig.Register("backlog", 2, func(tx *store.TxStore) error {
		ctx := context.Background()
		// id를 AUTOINCREMENT로 변경 — 삭제된 ID 재사용 방지
		_, err := tx.Exec(ctx, `
			CREATE TABLE backlog_items_new (
				id          INTEGER PRIMARY KEY AUTOINCREMENT,
				title       TEXT    NOT NULL,
				severity    TEXT    NOT NULL,
				timeframe   TEXT    NOT NULL,
				scope       TEXT    NOT NULL,
				type        TEXT    NOT NULL,
				description TEXT    NOT NULL,
				related     TEXT,
				position    INTEGER NOT NULL,
				status      TEXT    NOT NULL DEFAULT 'open',
				resolution  TEXT,
				resolved_at TEXT,
				created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
				updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
			);
			INSERT INTO backlog_items_new SELECT * FROM backlog_items;
			DROP TABLE backlog_items;
			ALTER TABLE backlog_items_new RENAME TO backlog_items;
		`)
		return err
	})
	mig.Register("backlog", 3, func(tx *store.TxStore) error {
		ctx := context.Background()
		// 1. 기존 데이터 대문자 정규화
		updates := []string{
			`UPDATE backlog_items SET status = UPPER(status) WHERE status != UPPER(status)`,
			`UPDATE backlog_items SET severity = UPPER(severity) WHERE severity != UPPER(severity)`,
			`UPDATE backlog_items SET type = UPPER(REPLACE(type, '-', '_')) WHERE type != UPPER(REPLACE(type, '-', '_'))`,
		}
		for _, q := range updates {
			if _, err := tx.Exec(ctx, q); err != nil {
				return fmt.Errorf("normalize: %w", err)
			}
		}

		// scope: 쉼표 구분 각각 정규화 (Go에서 처리)
		rows, err := tx.Query(ctx, "SELECT id, scope FROM backlog_items")
		if err != nil {
			return err
		}
		defer rows.Close()
		type idScope struct {
			id    int
			scope string
		}
		var pairs []idScope
		for rows.Next() {
			var p idScope
			if scanErr := rows.Scan(&p.id, &p.scope); scanErr != nil {
				return fmt.Errorf("scan scope: %w", scanErr)
			}
			pairs = append(pairs, p)
		}
		rows.Close()
		for _, p := range pairs {
			normalized := NormalizeScope(p.scope)
			if normalized != p.scope {
				if _, execErr := tx.Exec(ctx, "UPDATE backlog_items SET scope = ? WHERE id = ?", normalized, p.id); execErr != nil {
					return fmt.Errorf("update scope id=%d: %w", p.id, execErr)
				}
			}
		}

		// resolution 오염 데이터 정리
		resRows, err := tx.Query(ctx, "SELECT id, resolution FROM backlog_items WHERE resolution IS NOT NULL")
		if err != nil {
			return err
		}
		defer resRows.Close()
		type idRes struct {
			id  int
			res string
		}
		var resPairs []idRes
		for resRows.Next() {
			var p idRes
			if scanErr := resRows.Scan(&p.id, &p.res); scanErr != nil {
				return fmt.Errorf("scan resolution: %w", scanErr)
			}
			resPairs = append(resPairs, p)
		}
		resRows.Close()
		for _, p := range resPairs {
			normalized := NormalizeResolution(p.res)
			if normalized != p.res {
				if _, execErr := tx.Exec(ctx, "UPDATE backlog_items SET resolution = ? WHERE id = ?", normalized, p.id); execErr != nil {
					return fmt.Errorf("update resolution id=%d: %w", p.id, execErr)
				}
			}
		}

		// 2. 스키마 재생성 (DEFAULT 'OPEN')
		_, err = tx.Exec(ctx, `
			CREATE TABLE backlog_items_v3 (
				id          INTEGER PRIMARY KEY AUTOINCREMENT,
				title       TEXT    NOT NULL,
				severity    TEXT    NOT NULL,
				timeframe   TEXT    NOT NULL,
				scope       TEXT    NOT NULL,
				type        TEXT    NOT NULL,
				description TEXT    NOT NULL,
				related     TEXT,
				position    INTEGER NOT NULL,
				status      TEXT    NOT NULL DEFAULT 'OPEN',
				resolution  TEXT,
				resolved_at TEXT,
				created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
				updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
			);
			INSERT INTO backlog_items_v3 SELECT * FROM backlog_items;
			DROP TABLE backlog_items;
			ALTER TABLE backlog_items_v3 RENAME TO backlog_items;
		`)
		return err
	})
}

// RegisterRoutes registers all backlog action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("add", daemon.Typed(m.handleAdd))
	reg.Handle("list", m.handleList) // custom null-check logic
	reg.Handle("get", daemon.Typed(m.handleGet))
	reg.Handle("resolve", daemon.Typed(m.handleResolve))
	reg.Handle("check", daemon.Typed(m.handleCheck))
	reg.Handle("next-id", daemon.NoParams(m.handleNextID))
	reg.Handle("export", daemon.NoParams(m.handleExport))
	reg.Handle("release", daemon.Typed(m.handleRelease))
	reg.Handle("update", daemon.Typed(m.handleUpdate))
	reg.Handle("fix", daemon.Typed(m.handleFix))
	reg.Handle("sync-import", daemon.Typed(m.handleSyncImport))
}

func (m *Module) OnStart(_ context.Context) error { return nil }
func (m *Module) OnStop() error                   { return nil }

// ── Route handlers ──

func (m *Module) handleAdd(ctx context.Context, p addParams, _ string) (any, error) {
	if err := m.manager.Add(ctx, &p.BacklogItem); err != nil {
		return nil, err
	}
	// --fix: add 직후 FIXING 전이 + 브랜치 연결
	if p.Fix && p.Branch != "" {
		if err := m.manager.Fix(ctx, p.BacklogItem.ID, p.Branch); err != nil {
			return nil, fmt.Errorf("backlog.add: auto-fix: %w", err)
		}
	}
	return map[string]any{"id": p.BacklogItem.ID, "position": p.BacklogItem.Position, "fixed": p.Fix}, nil
}

// handleList has custom null-check logic so it keeps the raw HandlerFunc signature.
func (m *Module) handleList(ctx context.Context, params json.RawMessage, _ string) (any, error) {
	var filter ListFilter
	if len(params) > 0 && string(params) != "null" {
		if err := json.Unmarshal(params, &filter); err != nil {
			return nil, fmt.Errorf("backlog.list: decode params: %w", err)
		}
	}
	items, err := m.manager.List(ctx, filter)
	if err != nil {
		return nil, err
	}
	return items, nil
}

func (m *Module) handleGet(ctx context.Context, p getParams, _ string) (any, error) {
	item, err := m.manager.Get(ctx, p.ID)
	if err != nil {
		return nil, err
	}
	return item, nil
}

func (m *Module) handleResolve(ctx context.Context, p resolveParams, _ string) (any, error) {
	if err := m.manager.Resolve(ctx, p.ID, p.Resolution); err != nil {
		return nil, err
	}
	return map[string]string{"status": "resolved"}, nil
}

func (m *Module) handleCheck(ctx context.Context, p checkParams, _ string) (any, error) {
	exists, status, err := m.manager.Check(ctx, p.ID)
	if err != nil {
		return nil, err
	}
	return map[string]any{"exists": exists, "status": status}, nil
}

func (m *Module) handleNextID(ctx context.Context, _ string) (any, error) {
	id, err := m.manager.NextID(ctx)
	if err != nil {
		return nil, err
	}
	return map[string]int{"id": id}, nil
}

func (m *Module) handleExport(ctx context.Context, _ string) (any, error) {
	out, err := m.manager.ExportJSON(ctx)
	if err != nil {
		return nil, err
	}
	return map[string]string{"content": string(out)}, nil
}

func (m *Module) handleUpdate(ctx context.Context, p updateParams, _ string) (any, error) {
	if err := m.manager.Update(ctx, p.ID, p.Fields); err != nil {
		return nil, err
	}
	return map[string]string{"status": "updated"}, nil
}

func (m *Module) handleRelease(ctx context.Context, p releaseParams, _ string) (any, error) {
	if err := m.manager.Release(ctx, p.ID, p.Reason, p.Branch); err != nil {
		return nil, err
	}
	return map[string]string{"status": "released"}, nil
}

func (m *Module) handleFix(ctx context.Context, p fixParams, _ string) (any, error) {
	if err := m.manager.Fix(ctx, p.ID, p.Branch); err != nil {
		return nil, err
	}
	return map[string]string{"status": "fixing"}, nil
}

func (m *Module) handleSyncImport(ctx context.Context, p syncImportParams, _ string) (any, error) {
	if p.JSONData == "" {
		return map[string]any{"imported": 0}, nil
	}
	items, err := ParseBacklogJSON([]byte(p.JSONData))
	if err != nil {
		return nil, fmt.Errorf("backlog.sync-import: parse: %w", err)
	}
	n, err := m.manager.ImportItems(ctx, items)
	if err != nil {
		return nil, fmt.Errorf("backlog.sync-import: import: %w", err)
	}
	return map[string]any{"imported": n}, nil
}
