// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import "testing"

func TestMigrate_NewDB(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("test_module", 1, func(tx *TxStore) error {
		_, err := tx.Exec("CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatalf("Migrate() error: %v", err)
	}

	_, err := s.Exec("INSERT INTO test_items (id) VALUES (1)")
	if err != nil {
		t.Fatalf("table not created: %v", err)
	}
}

func TestMigrate_Idempotent(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	called := 0
	m := NewMigrator(s)
	m.Register("test_module", 1, func(tx *TxStore) error {
		called++
		_, err := tx.Exec("CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
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

	m := NewMigrator(s)
	m.Register("mod", 1, func(tx *TxStore) error {
		_, err := tx.Exec("CREATE TABLE items (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("mod", 2, func(tx *TxStore) error {
		_, err := tx.Exec("ALTER TABLE items ADD COLUMN name TEXT")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}

	_, err := s.Exec("INSERT INTO items (id, name) VALUES (1, 'test')")
	if err != nil {
		t.Fatalf("v2 migration failed: %v", err)
	}
}

func TestMigrate_DataIntegrity(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("mod", 1, func(tx *TxStore) error {
		_, err := tx.Exec("CREATE TABLE items (id INTEGER PRIMARY KEY, title TEXT NOT NULL)")
		return err
	})

	// Apply v1 and insert data.
	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Exec("INSERT INTO items (id, title) VALUES (1, 'first'), (2, 'second')"); err != nil {
		t.Fatalf("v1 insert: %v", err)
	}

	// Register v2 that recreates the table with a new column (like backlog v2 AUTOINCREMENT migration).
	m.Register("mod", 2, func(tx *TxStore) error {
		if _, err := tx.Exec(`CREATE TABLE items_new (
			id    INTEGER PRIMARY KEY AUTOINCREMENT,
			title TEXT NOT NULL,
			extra TEXT DEFAULT ''
		)`); err != nil {
			return err
		}
		if _, err := tx.Exec("INSERT INTO items_new (id, title) SELECT id, title FROM items"); err != nil {
			return err
		}
		if _, err := tx.Exec("DROP TABLE items"); err != nil {
			return err
		}
		_, err := tx.Exec("ALTER TABLE items_new RENAME TO items")
		return err
	})

	// Apply v2 and verify data survived.
	if err := m.Migrate(); err != nil {
		t.Fatalf("v2 migrate: %v", err)
	}

	var count int
	if err := s.QueryRow("SELECT COUNT(*) FROM items").Scan(&count); err != nil {
		t.Fatalf("count query: %v", err)
	}
	if count != 2 {
		t.Errorf("data lost: got %d rows, want 2", count)
	}

	var title string
	if err := s.QueryRow("SELECT title FROM items WHERE id = 1").Scan(&title); err != nil {
		t.Fatalf("row query: %v", err)
	}
	if title != "first" {
		t.Errorf("data corrupted: got %q, want %q", title, "first")
	}

	// Verify new column is accessible.
	if _, err := s.Exec("UPDATE items SET extra = 'test' WHERE id = 1"); err != nil {
		t.Fatalf("new column not accessible: %v", err)
	}
}

func TestMigrate_MultipleModules(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("alpha", 1, func(tx *TxStore) error {
		_, err := tx.Exec("CREATE TABLE alpha_t (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("beta", 1, func(tx *TxStore) error {
		_, err := tx.Exec("CREATE TABLE beta_t (id INTEGER PRIMARY KEY)")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}

	if _, err := s.Exec("INSERT INTO alpha_t (id) VALUES (1)"); err != nil {
		t.Fatal("alpha table missing")
	}
	if _, err := s.Exec("INSERT INTO beta_t (id) VALUES (1)"); err != nil {
		t.Fatal("beta table missing")
	}
}
