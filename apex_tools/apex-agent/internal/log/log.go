// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"io"
	"log/slog"
	"os"
	"strings"
)

// LogConfig is passed from the config package.
type LogConfig struct {
	Level  string
	Writer io.Writer // nil = os.Stderr (for testing, override with buffer)
}

// Init sets up the global logger. Call once at daemon start.
func Init(cfg LogConfig) {
	w := cfg.Writer
	if w == nil {
		w = os.Stderr
	}

	level := parseLevel(cfg.Level)
	handler := slog.NewTextHandler(w, &slog.HandlerOptions{Level: level})
	slog.SetDefault(slog.New(handler))
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
// IMPORTANT: resolves slog.Default() at each call, NOT at creation time.
// This ensures package-level `var ml = log.WithModule("handoff")` works
// correctly even though Init() hasn't been called yet at var-init time.
type ModuleLogger struct {
	name string
}

func WithModule(name string) *ModuleLogger {
	return &ModuleLogger{name: name}
}

func (ml *ModuleLogger) Debug(msg string, args ...any) {
	slog.Default().With("module", ml.name).Debug(msg, args...)
}
func (ml *ModuleLogger) Info(msg string, args ...any) {
	slog.Default().With("module", ml.name).Info(msg, args...)
}
func (ml *ModuleLogger) Warn(msg string, args ...any) {
	slog.Default().With("module", ml.name).Warn(msg, args...)
}
func (ml *ModuleLogger) Error(msg string, args ...any) {
	slog.Default().With("module", ml.name).Error(msg, args...)
}
func (ml *ModuleLogger) Audit(msg string, args ...any) {
	slog.Default().With("module", ml.name).Info(msg, append(args, "audit", true)...)
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
