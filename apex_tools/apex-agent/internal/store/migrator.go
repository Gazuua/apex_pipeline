// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"fmt"
	"sort"
)

type MigrateFunc func(tx *TxStore) error

type migration struct {
	module  string
	version int
	fn      MigrateFunc
}

type Migrator struct {
	store      *Store
	migrations []migration
}

func NewMigrator(s *Store) *Migrator {
	return &Migrator{store: s}
}

func (m *Migrator) Register(module string, version int, fn MigrateFunc) {
	m.migrations = append(m.migrations, migration{module, version, fn})
}

func (m *Migrator) Migrate() error {
	ctx := context.Background()
	_, err := m.store.Exec(ctx, `CREATE TABLE IF NOT EXISTS _migrations (
		module  TEXT NOT NULL,
		version INTEGER NOT NULL,
		PRIMARY KEY (module, version)
	)`)
	if err != nil {
		return fmt.Errorf("create _migrations: %w", err)
	}

	sort.Slice(m.migrations, func(i, j int) bool {
		if m.migrations[i].module != m.migrations[j].module {
			return m.migrations[i].module < m.migrations[j].module
		}
		return m.migrations[i].version < m.migrations[j].version
	})

	for _, mig := range m.migrations {
		var count int
		row := m.store.QueryRow(ctx,
			"SELECT COUNT(*) FROM _migrations WHERE module=? AND version=?",
			mig.module, mig.version,
		)
		if err := row.Scan(&count); err != nil {
			return err
		}
		if count > 0 {
			continue
		}

		// 각 마이그레이션을 트랜잭션으로 감싸서 원자적으로 실행
		if err := m.store.RunInTx(ctx, func(tx *TxStore) error {
			if err := mig.fn(tx); err != nil {
				return fmt.Errorf("migrate %s v%d: %w", mig.module, mig.version, err)
			}
			if _, err := tx.Exec(ctx,
				"INSERT INTO _migrations (module, version) VALUES (?, ?)",
				mig.module, mig.version,
			); err != nil {
				return err
			}
			return nil
		}); err != nil {
			return err
		}
	}
	return nil
}
