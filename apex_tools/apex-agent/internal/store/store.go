// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package store

import (
	"context"
	"database/sql"

	_ "modernc.org/sqlite"
)

type Store struct {
	db *sql.DB
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

func (s *Store) Close() error                                       { return s.db.Close() }
func (s *Store) Exec(query string, args ...any) (sql.Result, error) { return s.db.Exec(query, args...) }
func (s *Store) Query(query string, args ...any) (*sql.Rows, error) { return s.db.Query(query, args...) }
func (s *Store) QueryRow(query string, args ...any) *sql.Row        { return s.db.QueryRow(query, args...) }
func (s *Store) BeginTx(ctx context.Context) (*sql.Tx, error)       { return s.db.BeginTx(ctx, nil) }
