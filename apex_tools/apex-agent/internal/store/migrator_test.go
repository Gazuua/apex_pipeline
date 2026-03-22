// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import "testing"

func TestMigrate_NewDB(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("test_module", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
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
	m.Register("test_module", 1, func(s *Store) error {
		called++
		_, err := s.Exec("CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
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
	m.Register("mod", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE items (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("mod", 2, func(s *Store) error {
		_, err := s.Exec("ALTER TABLE items ADD COLUMN name TEXT")
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

func TestMigrate_MultipleModules(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("alpha", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE alpha_t (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("beta", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE beta_t (id INTEGER PRIMARY KEY)")
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
