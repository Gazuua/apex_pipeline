// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestDailyWriter_CreatesFile(t *testing.T) {
	dir := t.TempDir()
	w, err := NewDailyWriter(DailyWriterConfig{Dir: dir})
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	today := time.Now().Format("20060102")
	if _, err := w.Write([]byte("hello\n")); err != nil {
		t.Fatal(err)
	}

	expected := filepath.Join(dir, today+".log")
	if _, err := os.Stat(expected); err != nil {
		t.Errorf("expected log file %s to exist: %v", expected, err)
	}

	data, _ := os.ReadFile(expected)
	if string(data) != "hello\n" {
		t.Errorf("file content = %q, want %q", data, "hello\n")
	}
}

func TestDailyWriter_RotatesOnDateChange(t *testing.T) {
	dir := t.TempDir()
	w, err := NewDailyWriter(DailyWriterConfig{Dir: dir})
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	// Freeze time to day1
	day1 := time.Date(2026, 3, 25, 10, 0, 0, 0, time.Local)
	w.nowFunc = func() time.Time { return day1 }

	if _, err := w.Write([]byte("day1\n")); err != nil {
		t.Fatal(err)
	}

	// Advance to day2
	day2 := time.Date(2026, 3, 26, 10, 0, 0, 0, time.Local)
	w.nowFunc = func() time.Time { return day2 }

	if _, err := w.Write([]byte("day2\n")); err != nil {
		t.Fatal(err)
	}

	// Verify both files exist
	f1 := filepath.Join(dir, "20260325.log")
	f2 := filepath.Join(dir, "20260326.log")

	data1, err := os.ReadFile(f1)
	if err != nil {
		t.Fatalf("day1 file missing: %v", err)
	}
	if string(data1) != "day1\n" {
		t.Errorf("day1 content = %q, want %q", data1, "day1\n")
	}

	data2, err := os.ReadFile(f2)
	if err != nil {
		t.Fatalf("day2 file missing: %v", err)
	}
	if string(data2) != "day2\n" {
		t.Errorf("day2 content = %q, want %q", data2, "day2\n")
	}
}

func TestDailyWriter_CleanupsOldFiles(t *testing.T) {
	dir := t.TempDir()
	w, err := NewDailyWriter(DailyWriterConfig{Dir: dir, MaxDays: 3})
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	// Create old files manually
	oldFiles := []string{"20260301.log", "20260310.log", "20260320.log"}
	for _, f := range oldFiles {
		os.WriteFile(filepath.Join(dir, f), []byte("old"), 0o644)
	}

	// Write on day 20260325 — files older than 3 days (< 20260322) should be removed
	w.nowFunc = func() time.Time { return time.Date(2026, 3, 25, 10, 0, 0, 0, time.Local) }
	if _, err := w.Write([]byte("today\n")); err != nil {
		t.Fatal(err)
	}

	// 20260301, 20260310, 20260320 are all < 20260322 → should be removed
	for _, f := range oldFiles {
		path := filepath.Join(dir, f)
		if _, err := os.Stat(path); err == nil {
			t.Errorf("expected old file %s to be cleaned up", f)
		}
	}

	// Current day file should still exist
	todayFile := filepath.Join(dir, "20260325.log")
	if _, err := os.Stat(todayFile); err != nil {
		t.Errorf("today's log file should exist: %v", err)
	}
}

func TestDailyWriter_Close(t *testing.T) {
	dir := t.TempDir()
	w, err := NewDailyWriter(DailyWriterConfig{Dir: dir})
	if err != nil {
		t.Fatal(err)
	}

	if _, err := w.Write([]byte("test\n")); err != nil {
		t.Fatal(err)
	}

	if err := w.Close(); err != nil {
		t.Fatal(err)
	}

	// Double close should not error
	if err := w.Close(); err != nil {
		t.Errorf("double close should not error: %v", err)
	}
}

func TestDailyWriter_NonDateFilesIgnored(t *testing.T) {
	dir := t.TempDir()
	w, err := NewDailyWriter(DailyWriterConfig{Dir: dir, MaxDays: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	// Create a non-date log file that should NOT be deleted
	nonDateFile := filepath.Join(dir, "apex-agent.log")
	os.WriteFile(nonDateFile, []byte("legacy"), 0o644)

	w.nowFunc = func() time.Time { return time.Date(2026, 3, 25, 10, 0, 0, 0, time.Local) }
	if _, err := w.Write([]byte("today\n")); err != nil {
		t.Fatal(err)
	}

	// Non-date file should still exist (not 8-digit name → cleanup ignores it)
	if _, err := os.Stat(nonDateFile); err != nil {
		t.Errorf("non-date file should not be cleaned up: %v", err)
	}
}
