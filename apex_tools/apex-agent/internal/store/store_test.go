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

	ctx := context.Background()
	_, err = s.Exec(ctx, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
	if err != nil {
		t.Fatalf("Exec CREATE TABLE error: %v", err)
	}

	_, err = s.Exec(ctx, "INSERT INTO test (name) VALUES (?)", "hello")
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

	ctx := context.Background()
	s.Exec(ctx, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
	s.Exec(ctx, "INSERT INTO test (name) VALUES (?)", "alice")
	s.Exec(ctx, "INSERT INTO test (name) VALUES (?)", "bob")

	rows, err := s.Query(ctx, "SELECT name FROM test ORDER BY id")
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

func TestRunInTx_CommitAndRollback(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	ctx := context.Background()
	s.Exec(ctx, "CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	// Commit via RunInTx
	err = s.RunInTx(ctx, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "INSERT INTO test (val) VALUES (?)", "committed")
		return err
	})
	if err != nil {
		t.Fatalf("RunInTx commit error: %v", err)
	}

	// Rollback via RunInTx
	err = s.RunInTx(ctx, func(tx *TxStore) error {
		tx.Exec(ctx, "INSERT INTO test (val) VALUES (?)", "rolled_back")
		return errors.New("rollback")
	})
	if err == nil {
		t.Fatal("expected error from rollback")
	}

	rows, _ := s.Query(ctx, "SELECT val FROM test")
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

	ctx := context.Background()
	s.Exec(ctx, "CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	err = s.RunInTx(ctx, func(tx *TxStore) error {
		_, err := tx.Exec(ctx, "INSERT INTO test (val) VALUES (?)", "committed")
		return err
	})
	if err != nil {
		t.Fatalf("RunInTx returned error: %v", err)
	}

	// Verify the row was committed
	row := s.QueryRow(ctx, "SELECT val FROM test WHERE val = ?", "committed")
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

	ctx := context.Background()
	s.Exec(ctx, "CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	testErr := errors.New("deliberate error")
	err = s.RunInTx(ctx, func(tx *TxStore) error {
		tx.Exec(ctx, "INSERT INTO test (val) VALUES (?)", "should_rollback")
		return testErr
	})
	if !errors.Is(err, testErr) {
		t.Fatalf("expected deliberate error, got: %v", err)
	}

	// Verify the row was NOT committed
	row := s.QueryRow(ctx, "SELECT COUNT(*) FROM test")
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

	ctx := context.Background()
	s.Exec(ctx, "CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	// Verify TxStore is properly isolated — it's a separate type from Store
	err = s.RunInTx(ctx, func(tx *TxStore) error {
		if tx.tx == nil {
			t.Error("tx.tx should be non-nil inside RunInTx callback")
		}
		_, err := tx.Exec(ctx, "INSERT INTO test (val) VALUES (?)", "isolated")
		return err
	})
	if err != nil {
		t.Fatalf("RunInTx returned error: %v", err)
	}
}

// ── Querier interface ──

func TestQuerier_StoreImplements(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	var q Querier = s // Store must satisfy Querier
	_ = q
}

func TestQuerier_TxStoreImplements(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.RunInTx(context.Background(), func(tx *TxStore) error {
		var q Querier = tx // TxStore must satisfy Querier
		_ = q
		return nil
	})
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
