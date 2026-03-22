// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"errors"
	"testing"
)

func TestOpen_InMemory(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatalf("Open(:memory:) error: %v", err)
	}
	defer s.Close()
}

func TestExec_CreateTable(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	_, err = s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
	if err != nil {
		t.Fatalf("Exec CREATE TABLE error: %v", err)
	}

	_, err = s.Exec("INSERT INTO test (name) VALUES (?)", "hello")
	if err != nil {
		t.Fatalf("Exec INSERT error: %v", err)
	}
}

func TestQuery_SelectRows(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
	s.Exec("INSERT INTO test (name) VALUES (?)", "alice")
	s.Exec("INSERT INTO test (name) VALUES (?)", "bob")

	rows, err := s.Query("SELECT name FROM test ORDER BY id")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()

	var names []string
	for rows.Next() {
		var name string
		rows.Scan(&name)
		names = append(names, name)
	}

	if len(names) != 2 || names[0] != "alice" || names[1] != "bob" {
		t.Errorf("got %v, want [alice bob]", names)
	}
}

func TestBeginTx_CommitAndRollback(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	// Commit
	tx, _ := s.BeginTx(context.Background())
	tx.Exec("INSERT INTO test (val) VALUES (?)", "committed")
	tx.Commit()

	// Rollback
	tx2, _ := s.BeginTx(context.Background())
	tx2.Exec("INSERT INTO test (val) VALUES (?)", "rolled_back")
	tx2.Rollback()

	rows, _ := s.Query("SELECT val FROM test")
	defer rows.Close()
	var count int
	for rows.Next() {
		count++
	}
	if count != 1 {
		t.Errorf("got %d rows, want 1 (only committed)", count)
	}
}

// ── RunInTx ──

func TestRunInTx_Commit(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	err = s.RunInTx(context.Background(), func(txs *Store) error {
		_, err := txs.Exec("INSERT INTO test (val) VALUES (?)", "committed")
		return err
	})
	if err != nil {
		t.Fatalf("RunInTx returned error: %v", err)
	}

	// Verify the row was committed
	row := s.QueryRow("SELECT val FROM test WHERE val = ?", "committed")
	var val string
	if err := row.Scan(&val); err != nil {
		t.Fatalf("expected row to be committed, got error: %v", err)
	}
	if val != "committed" {
		t.Errorf("got %q, want %q", val, "committed")
	}
}

func TestRunInTx_Rollback(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	testErr := errors.New("deliberate error")
	err = s.RunInTx(context.Background(), func(txs *Store) error {
		txs.Exec("INSERT INTO test (val) VALUES (?)", "should_rollback")
		return testErr
	})
	if !errors.Is(err, testErr) {
		t.Fatalf("expected deliberate error, got: %v", err)
	}

	// Verify the row was NOT committed
	row := s.QueryRow("SELECT COUNT(*) FROM test")
	var count int
	if err := row.Scan(&count); err != nil {
		t.Fatalf("count query failed: %v", err)
	}
	if count != 0 {
		t.Errorf("expected 0 rows after rollback, got %d", count)
	}
}

func TestRunInTx_TxStoreIsolation(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	// Verify the original Store's tx field remains nil after RunInTx
	err = s.RunInTx(context.Background(), func(txs *Store) error {
		// Inside the callback, txs should have a non-nil tx
		if txs.tx == nil {
			t.Error("txs.tx should be non-nil inside RunInTx callback")
		}
		// The original Store's tx should still be nil
		if s.tx != nil {
			t.Error("original Store's tx should remain nil inside RunInTx callback")
		}
		_, err := txs.Exec("INSERT INTO test (val) VALUES (?)", "isolated")
		return err
	})
	if err != nil {
		t.Fatalf("RunInTx returned error: %v", err)
	}

	// After RunInTx, original Store's tx should still be nil
	if s.tx != nil {
		t.Error("original Store's tx should remain nil after RunInTx")
	}
}

// ── NullableString ──

func TestNullableString(t *testing.T) {
	tests := []struct {
		name  string
		input string
		isNil bool
	}{
		{"empty string returns nil", "", true},
		{"non-empty returns value", "hello", false},
		{"whitespace returns value", " ", false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			result := NullableString(tc.input)
			if tc.isNil {
				if result != nil {
					t.Errorf("NullableString(%q) = %v, want nil", tc.input, result)
				}
			} else {
				if result == nil {
					t.Errorf("NullableString(%q) = nil, want %q", tc.input, tc.input)
				}
				if s, ok := result.(string); ok && s != tc.input {
					t.Errorf("NullableString(%q) = %q, want %q", tc.input, s, tc.input)
				}
			}
		})
	}
}
