// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"database/sql"

	_ "modernc.org/sqlite"
)

type Store struct {
	db *sql.DB
	tx *sql.Tx // non-nil when inside RunInTx — routes Exec/Query through transaction
}

func Open(dsn string) (*Store, error) {
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
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
	if s.tx != nil {
		return s.tx.Exec(query, args...)
	}
	return s.db.Exec(query, args...)
}

func (s *Store) Query(query string, args ...any) (*sql.Rows, error) {
	if s.tx != nil {
		return s.tx.Query(query, args...)
	}
	return s.db.Query(query, args...)
}

func (s *Store) QueryRow(query string, args ...any) *sql.Row {
	if s.tx != nil {
		return s.tx.QueryRow(query, args...)
	}
	return s.db.QueryRow(query, args...)
}

func (s *Store) BeginTx(ctx context.Context) (*sql.Tx, error) { return s.db.BeginTx(ctx, nil) }

// RunInTx executes fn within a transaction. All Store.Exec/Query/QueryRow calls
// inside fn are routed through the transaction. Commits on success, rolls back on error.
func (s *Store) RunInTx(ctx context.Context, fn func() error) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	s.tx = tx
	defer func() { s.tx = nil }()

	if err := fn(); err != nil {
		tx.Rollback() //nolint:errcheck
		return err
	}
	return tx.Commit()
}
