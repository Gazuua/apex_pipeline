// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"io"
	"log/slog"
	"os"
	"strings"
	"sync"
	"sync/atomic"
)

// LogConfig is passed from the config package.
type LogConfig struct {
	Level  string
	Writer io.Writer // nil = os.Stderr (for testing, override with buffer)
}

// initGeneration is bumped every time Init() is called.
// ModuleLogger uses this to detect when its cached logger is stale.
var initGeneration atomic.Int64

// Init sets up the global logger. Call once at daemon start.
func Init(cfg LogConfig) {
	w := cfg.Writer
	if w == nil {
		w = os.Stderr
	}

	level := parseLevel(cfg.Level)
	handler := slog.NewTextHandler(w, &slog.HandlerOptions{
		Level:     level,
		AddSource: true,
	})
	slog.SetDefault(slog.New(handler))

	// Bump generation to invalidate all cached ModuleLoggers.
	initGeneration.Add(1)
}

// Package-level wrapper functions (spdlog-style ergonomics)
func Debug(msg string, args ...any) { slog.Debug(msg, args...) }
func Info(msg string, args ...any)  { slog.Info(msg, args...) }
func Warn(msg string, args ...any)  { slog.Warn(msg, args...) }
func Error(msg string, args ...any) { slog.Error(msg, args...) }

func Audit(msg string, args ...any) {
	slog.Info(msg, append(args, "audit", true)...)
}

// ModuleLogger adds a "module" field to all log entries.
// Caches the derived *slog.Logger after first use (or after Init() is called)
// to avoid per-call allocation from slog.Default().With().
type ModuleLogger struct {
	name string

	mu     sync.Mutex
	cached *slog.Logger
	gen    int64 // generation when cached was created
}

// WithModule creates a ModuleLogger. The cached logger is lazily built on first
// use and automatically invalidated when Init() bumps the generation counter.
func WithModule(name string) *ModuleLogger {
	return &ModuleLogger{name: name}
}

// logger returns the cached *slog.Logger, rebuilding it if Init() was called
// since the last cache.
func (ml *ModuleLogger) logger() *slog.Logger {
	currentGen := initGeneration.Load()

	ml.mu.Lock()
	defer ml.mu.Unlock()

	if ml.cached == nil || ml.gen != currentGen {
		ml.cached = slog.Default().With("module", ml.name)
		ml.gen = currentGen
	}
	return ml.cached
}

func (ml *ModuleLogger) Debug(msg string, args ...any) {
	ml.logger().Debug(msg, args...)
}
func (ml *ModuleLogger) Info(msg string, args ...any) {
	ml.logger().Info(msg, args...)
}
func (ml *ModuleLogger) Warn(msg string, args ...any) {
	ml.logger().Warn(msg, args...)
}
func (ml *ModuleLogger) Error(msg string, args ...any) {
	ml.logger().Error(msg, args...)
}
func (ml *ModuleLogger) Audit(msg string, args ...any) {
	ml.logger().Info(msg, append(args, "audit", true)...)
}

func parseLevel(s string) slog.Level {
	switch strings.ToLower(s) {
	case "debug":
		return slog.LevelDebug
	case "warn":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}
