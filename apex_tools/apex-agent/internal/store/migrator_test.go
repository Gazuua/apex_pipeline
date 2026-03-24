// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"fmt"
	"testing"
)

func TestMigrate_NewDB(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	ctx := context.Background()
	m.Register("test_module", 1, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatalf("Migrate() error: %v", err)
	}

	_, err := s.Exec(ctx, "INSERT INTO test_items (id) VALUES (1)")
	if err != nil {
		t.Fatalf("table not created: %v", err)
	}
}

func TestMigrate_Idempotent(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	ctx := context.Background()
	called := 0
	m := NewMigrator(s)
	m.Register("test_module", 1, func(tx *TxStore) error {
		called++
		_, err := tx.Exec(ctx, "CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
		return err
	})

	m.Migrate()
	m.Migrate()

	if called != 1 {
		t.Errorf("migration called %d times, want 1", called)
	}
}

func TestMigrate_MultiVersion(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	ctx := context.Background()
	m := NewMigrator(s)
	m.Register("mod", 1, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "CREATE TABLE items (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("mod", 2, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "ALTER TABLE items ADD COLUMN name TEXT")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}

	_, err := s.Exec(ctx, "INSERT INTO items (id, name) VALUES (1, 'test')")
	if err != nil {
		t.Fatalf("v2 migration failed: %v", err)
	}
}

func TestMigrate_DataIntegrity(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	ctx := context.Background()
	m := NewMigrator(s)
	m.Register("mod", 1, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "CREATE TABLE items (id INTEGER PRIMARY KEY, title TEXT NOT NULL)")
		return err
	})

	// Apply v1 and insert data.
	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Exec(ctx, "INSERT INTO items (id, title) VALUES (1, 'first'), (2, 'second')"); err != nil {
		t.Fatalf("v1 insert: %v", err)
	}

	// Register v2 that recreates the table with a new column (like backlog v2 AUTOINCREMENT migration).
	m.Register("mod", 2, func(tx *TxStore) error {
		if _, err := tx.Exec(ctx, `CREATE TABLE items_new (
			id    INTEGER PRIMARY KEY AUTOINCREMENT,
			title TEXT NOT NULL,
			extra TEXT DEFAULT ''
		)`); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, "INSERT INTO items_new (id, title) SELECT id, title FROM items"); err != nil {
			return err
		}
		if _, err := tx.Exec(ctx, "DROP TABLE items"); err != nil {
			return err
		}
		_, err := tx.Exec(ctx, "ALTER TABLE items_new RENAME TO items")
		return err
	})

	// Apply v2 and verify data survived.
	if err := m.Migrate(); err != nil {
		t.Fatalf("v2 migrate: %v", err)
	}

	var count int
	if err := s.QueryRow(ctx, "SELECT COUNT(*) FROM items").Scan(&count); err != nil {
		t.Fatalf("count query: %v", err)
	}
	if count != 2 {
		t.Errorf("data lost: got %d rows, want 2", count)
	}

	var title string
	if err := s.QueryRow(ctx, "SELECT title FROM items WHERE id = 1").Scan(&title); err != nil {
		t.Fatalf("row query: %v", err)
	}
	if title != "first" {
		t.Errorf("data corrupted: got %q, want %q", title, "first")
	}

	// Verify new column is accessible.
	if _, err := s.Exec(ctx, "UPDATE items SET extra = 'test' WHERE id = 1"); err != nil {
		t.Fatalf("new column not accessible: %v", err)
	}
}

func TestMigrate_FailureRollback(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	ctx := context.Background()
	m := NewMigrator(s)

	// v1: 테이블 생성 + 데이터 삽입
	m.Register("mod", 1, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")
		return err
	})

	// v1 적용 + 데이터 삽입
	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Exec(ctx, "INSERT INTO items (id, name) VALUES (1, 'preserved')"); err != nil {
		t.Fatalf("v1 insert: %v", err)
	}

	// v2: 의도적 에러 반환
	m.Register("mod", 2, func(tx *TxStore) error {
		// 새 컬럼 추가 시도 후 에러 반환 — 트랜잭션이 롤백되어야 함
		if _, err := tx.Exec(ctx, "ALTER TABLE items ADD COLUMN extra TEXT"); err != nil {
			return err
		}
		return fmt.Errorf("intentional v2 failure")
	})

	// v2 마이그레이션은 실패해야 함
	if err := m.Migrate(); err == nil {
		t.Fatal("expected error from failing v2 migration")
	}

	// v1 데이터는 보존되어야 함
	var name string
	if err := s.QueryRow(ctx, "SELECT name FROM items WHERE id = 1").Scan(&name); err != nil {
		t.Fatalf("v1 data should be preserved: %v", err)
	}
	if name != "preserved" {
		t.Errorf("v1 data corrupted: got %q, want %q", name, "preserved")
	}

	// v2 스키마(extra 컬럼)는 롤백되어야 함 — extra 컬럼 접근 시 에러
	_, err := s.Exec(ctx, "UPDATE items SET extra = 'test' WHERE id = 1")
	if err == nil {
		t.Error("v2 schema should have been rolled back; extra column should not exist")
	}

	// _migrations에 v2 기록이 없어야 함
	var count int
	if err := s.QueryRow(ctx, "SELECT COUNT(*) FROM _migrations WHERE module='mod' AND version=2").Scan(&count); err != nil {
		t.Fatalf("query _migrations: %v", err)
	}
	if count != 0 {
		t.Error("v2 should not be recorded in _migrations after rollback")
	}
}

func TestMigrate_MultipleModules(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	ctx := context.Background()
	m := NewMigrator(s)
	m.Register("alpha", 1, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "CREATE TABLE alpha_t (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("beta", 1, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "CREATE TABLE beta_t (id INTEGER PRIMARY KEY)")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}

	if _, err := s.Exec(ctx, "INSERT INTO alpha_t (id) VALUES (1)"); err != nil {
		t.Fatal("alpha table missing")
	}
	if _, err := s.Exec(ctx, "INSERT INTO beta_t (id) VALUES (1)"); err != nil {
		t.Fatal("beta table missing")
	}
}
