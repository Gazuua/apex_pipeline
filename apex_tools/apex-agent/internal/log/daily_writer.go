// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// DailyWriter implements io.Writer with date-based log file rotation.
// Each day's logs go to a separate YYYYMMDD.log file in the configured directory.
// Old log files beyond MaxDays are automatically cleaned up on rotation.
type DailyWriter struct {
	dir     string // directory for log files
	maxDays int    // max days to retain (0 = unlimited)

	mu       sync.Mutex
	file     *os.File
	curDate  string // YYYYMMDD of currently open file
	nowFunc  func() time.Time
}

// DailyWriterConfig configures a DailyWriter.
type DailyWriterConfig struct {
	Dir     string // directory to write log files
	MaxDays int    // max days to retain old logs (0 = unlimited)
}

// NewDailyWriter creates a DailyWriter that writes to dir/YYYYMMDD.log.
// The directory is created if it does not exist.
func NewDailyWriter(cfg DailyWriterConfig) (*DailyWriter, error) {
	if err := os.MkdirAll(cfg.Dir, 0o755); err != nil {
		return nil, fmt.Errorf("create log directory %s: %w", cfg.Dir, err)
	}
	return &DailyWriter{
		dir:     cfg.Dir,
		maxDays: cfg.MaxDays,
		nowFunc: time.Now,
	}, nil
}

// Write implements io.Writer. Thread-safe.
// On date change, closes the current file and opens a new one.
func (w *DailyWriter) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()

	today := w.nowFunc().Format("20060102")
	if w.file == nil || w.curDate != today {
		if err := w.rotateLocked(today); err != nil {
			return 0, err
		}
	}
	return w.file.Write(p)
}

// Close closes the current log file.
func (w *DailyWriter) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.file != nil {
		err := w.file.Close()
		w.file = nil
		w.curDate = ""
		return err
	}
	return nil
}

// rotateLocked opens a new daily log file and cleans up old ones.
// Must be called with w.mu held.
func (w *DailyWriter) rotateLocked(date string) error {
	if w.file != nil {
		w.file.Close()
		w.file = nil
	}

	path := filepath.Join(w.dir, date+".log")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return fmt.Errorf("open log file %s: %w", path, err)
	}

	w.file = f
	w.curDate = date

	// Cleanup old files in background (best-effort).
	if w.maxDays > 0 {
		w.cleanupOldLocked()
	}

	return nil
}

// cleanupOldLocked removes log files older than maxDays.
// Must be called with w.mu held.
func (w *DailyWriter) cleanupOldLocked() {
	entries, err := os.ReadDir(w.dir)
	if err != nil {
		return
	}

	cutoff := w.nowFunc().AddDate(0, 0, -w.maxDays).Format("20060102")

	var logFiles []string
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		if !strings.HasSuffix(name, ".log") {
			continue
		}
		datePart := strings.TrimSuffix(name, ".log")
		// Validate it looks like a date (8 digits).
		if len(datePart) != 8 {
			continue
		}
		logFiles = append(logFiles, name)
	}

	sort.Strings(logFiles)

	for _, name := range logFiles {
		datePart := strings.TrimSuffix(name, ".log")
		if datePart < cutoff {
			os.Remove(filepath.Join(w.dir, name))
		}
	}
}
