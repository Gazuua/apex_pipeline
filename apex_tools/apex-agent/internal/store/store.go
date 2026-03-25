// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"database/sql"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	_ "modernc.org/sqlite"
)

var ml = log.WithModule("store")

// Querier is the common interface for Store and TxStore.
// Use this when a function needs to work with both transactional and non-transactional contexts.
type Querier interface {
	Exec(ctx context.Context, query string, args ...any) (sql.Result, error)
	Query(ctx context.Context, query string, args ...any) (*sql.Rows, error)
	QueryRow(ctx context.Context, query string, args ...any) *sql.Row
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
	ml.Info("opening database", "dsn", dsn)
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		ml.Error("database open failed", "dsn", dsn, "err", err)
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

func (s *Store) Close() error {
	ml.Info("closing database")
	return s.db.Close()
}

func (s *Store) Exec(ctx context.Context, query string, args ...any) (sql.Result, error) {
	return s.db.ExecContext(ctx, query, args...)
}

func (s *Store) Query(ctx context.Context, query string, args ...any) (*sql.Rows, error) {
	return s.db.QueryContext(ctx, query, args...)
}

func (s *Store) QueryRow(ctx context.Context, query string, args ...any) *sql.Row {
	return s.db.QueryRowContext(ctx, query, args...)
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
		if rbErr := tx.Rollback(); rbErr != nil {
			ml.Error("tx rollback failed", "rollback_err", rbErr, "original_err", err)
		}
		return err
	}
	return tx.Commit()
}

// --- TxStore methods ---

func (ts *TxStore) Exec(ctx context.Context, query string, args ...any) (sql.Result, error) {
	return ts.tx.ExecContext(ctx, query, args...)
}

func (ts *TxStore) Query(ctx context.Context, query string, args ...any) (*sql.Rows, error) {
	return ts.tx.QueryContext(ctx, query, args...)
}

func (ts *TxStore) QueryRow(ctx context.Context, query string, args ...any) *sql.Row {
	return ts.tx.QueryRowContext(ctx, query, args...)
}

// NullableString returns nil for empty strings, otherwise the string itself.
// Useful for nullable TEXT columns in SQLite.
func NullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}
