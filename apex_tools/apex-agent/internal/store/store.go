// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"database/sql"

	_ "modernc.org/sqlite"
)

// Querier is the common interface for Store and TxStore.
// Use this when a function needs to work with both transactional and non-transactional contexts.
type Querier interface {
	Exec(query string, args ...any) (sql.Result, error)
	Query(query string, args ...any) (*sql.Rows, error)
	QueryRow(query string, args ...any) *sql.Row
}

// Store wraps a SQLite database for non-transactional operations.
// Use RunInTx for transactional operations — the callback receives a TxStore.
type Store struct {
	db *sql.DB
}

// TxStore wraps a sql.Tx for transactional operations.
// Only exists inside RunInTx callbacks — prevents misuse of closed transactions.
type TxStore struct {
	tx *sql.Tx
}

func Open(dsn string) (*Store, error) {
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	// In-memory SQLite: each connection gets its own database.
	// Limit to 1 connection so all goroutines share the same in-memory DB.
	if dsn == ":memory:" {
		db.SetMaxOpenConns(1)
	}
	if _, err := db.Exec("PRAGMA journal_mode=WAL"); err != nil {
		db.Close()
		return nil, err
	}
	if _, err := db.Exec("PRAGMA busy_timeout=5000"); err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db}, nil
}

func (s *Store) Close() error { return s.db.Close() }

func (s *Store) Exec(query string, args ...any) (sql.Result, error) {
	return s.db.Exec(query, args...)
}

func (s *Store) Query(query string, args ...any) (*sql.Rows, error) {
	return s.db.Query(query, args...)
}

func (s *Store) QueryRow(query string, args ...any) *sql.Row {
	return s.db.QueryRow(query, args...)
}

// RunInTx executes fn within a transaction. A TxStore is passed to fn —
// all Exec/Query/QueryRow calls on it go through the transaction.
// Commits on success, rolls back on error.
func (s *Store) RunInTx(ctx context.Context, fn func(tx *TxStore) error) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	txs := &TxStore{tx: tx}

	if err := fn(txs); err != nil {
		tx.Rollback() //nolint:errcheck
		return err
	}
	return tx.Commit()
}

// --- TxStore methods ---

func (ts *TxStore) Exec(query string, args ...any) (sql.Result, error) {
	return ts.tx.Exec(query, args...)
}

func (ts *TxStore) Query(query string, args ...any) (*sql.Rows, error) {
	return ts.tx.Query(query, args...)
}

func (ts *TxStore) QueryRow(query string, args ...any) *sql.Row {
	return ts.tx.QueryRow(query, args...)
}

// NullableString returns nil for empty strings, otherwise the string itself.
// Useful for nullable TEXT columns in SQLite.
func NullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}
