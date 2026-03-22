// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
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
