// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync/atomic"
	"testing"
	"time"
)

func TestWatchBuildLog_KillsStaleProcess(t *testing.T) {
	// 장시간 프로세스를 시작하고, 로그에 아무것도 안 쓰면 watchdog이 kill하는지 검증.
	logDir := t.TempDir()
	logPath := filepath.Join(logDir, "test_build.log")
	if err := os.WriteFile(logPath, []byte{}, 0o644); err != nil {
		t.Fatalf("create log file: %v", err)
	}

	// 장시간 대기 프로세스 시작 (OS별 분기)
	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		// Windows: ping -n 60 127.0.0.1 (약 60초 대기)
		cmd = exec.Command("ping", "-n", "60", "127.0.0.1")
	} else {
		cmd = exec.Command("sleep", "60")
	}
	cmd.Stdout = nil
	cmd.Stderr = nil
	if err := cmd.Start(); err != nil {
		t.Fatalf("start long-running process: %v", err)
	}

	var killed atomic.Bool
	timeout := 200 * time.Millisecond
	pollInterval := 50 * time.Millisecond

	done := make(chan struct{})
	go func() {
		defer close(done)
		watchBuildLogWithTimeout(logPath, cmd.Process, &killed, timeout, pollInterval)
	}()

	// watchdog이 프로세스를 kill할 때까지 대기 (최대 2초)
	select {
	case <-done:
		// watchdog 종료됨
	case <-time.After(2 * time.Second):
		t.Fatal("watchdog did not kill stale process within 2s")
	}

	if !killed.Load() {
		t.Error("expected killed flag to be set")
	}

	// 프로세스가 실제로 종료되었는지 확인
	waitErr := cmd.Wait()
	if waitErr == nil {
		t.Error("expected process to be killed (non-zero exit)")
	}
}

func TestWatchBuildLog_ExitsWhenKilledExternally(t *testing.T) {
	// 프로세스가 외부에서 종료된 경우 (killed flag 설정), watchdog이 즉시 반환하는지 검증.
	logDir := t.TempDir()
	logPath := filepath.Join(logDir, "test_build.log")
	os.WriteFile(logPath, []byte("initial output\n"), 0o644)

	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		cmd = exec.Command("ping", "-n", "60", "127.0.0.1")
	} else {
		cmd = exec.Command("sleep", "60")
	}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start process: %v", err)
	}
	defer func() {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
	}()

	var killed atomic.Bool
	timeout := 5 * time.Second // 충분히 길게 — watchdog이 timeout으로 kill하면 안 됨
	pollInterval := 50 * time.Millisecond

	done := make(chan struct{})
	go func() {
		defer close(done)
		watchBuildLogWithTimeout(logPath, cmd.Process, &killed, timeout, pollInterval)
	}()

	// 100ms 후 외부에서 killed 플래그 설정 (프로세스 자연 종료 시뮬레이션)
	time.Sleep(100 * time.Millisecond)
	killed.Store(true)

	select {
	case <-done:
		// 정상 — killed 플래그 감지 후 반환
	case <-time.After(2 * time.Second):
		t.Fatal("watchdog should exit promptly when killed flag is set")
	}
}

func TestWatchBuildLog_ActiveLogPreventsKill(t *testing.T) {
	// 로그 파일에 계속 쓰면 watchdog이 kill하지 않는지 검증.
	logDir := t.TempDir()
	logPath := filepath.Join(logDir, "test_build.log")
	os.WriteFile(logPath, []byte{}, 0o644)

	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		cmd = exec.Command("ping", "-n", "60", "127.0.0.1")
	} else {
		cmd = exec.Command("sleep", "60")
	}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start process: %v", err)
	}
	defer func() {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
	}()

	var killed atomic.Bool
	timeout := 300 * time.Millisecond
	pollInterval := 50 * time.Millisecond

	done := make(chan struct{})
	go func() {
		defer close(done)
		watchBuildLogWithTimeout(logPath, cmd.Process, &killed, timeout, pollInterval)
	}()

	// 로그 파일에 주기적으로 쓰기 (timeout보다 짧은 간격)
	writeDone := make(chan struct{})
	go func() {
		defer close(writeDone)
		f, err := os.OpenFile(logPath, os.O_APPEND|os.O_WRONLY, 0o644)
		if err != nil {
			return
		}
		defer f.Close()
		for i := 0; i < 8; i++ {
			time.Sleep(100 * time.Millisecond)
			f.WriteString("log output\n")
			f.Sync()
		}
	}()

	// 쓰기 완료 대기 (800ms — timeout 300ms보다 충분히 김)
	<-writeDone

	// 쓰기 중에는 kill되지 않아야 함
	if killed.Load() {
		t.Error("watchdog should NOT kill process while log is actively written")
	}

	// 정리: killed 플래그로 watchdog 종료
	killed.Store(true)
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("watchdog did not exit after killed flag set")
	}
}
