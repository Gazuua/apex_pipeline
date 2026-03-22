// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"io"
	"log/slog"
	"os"
)

type Logger = *slog.Logger

type options struct {
	writer io.Writer
	level  slog.Level
}

type Option func(*options)

func WithWriter(w io.Writer) Option { return func(o *options) { o.writer = w } }
func WithLevel(l slog.Level) Option { return func(o *options) { o.level = l } }

func New(opts ...Option) Logger {
	o := &options{
		writer: os.Stderr,
		level:  slog.LevelInfo,
	}
	for _, opt := range opts {
		opt(o)
	}
	return slog.New(slog.NewTextHandler(o.writer, &slog.HandlerOptions{
		Level: o.level,
	}))
}
