// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func testSocketAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-daemon-test`
	}
	return "/tmp/apex-agent-daemon-test.sock"
}

func TestDaemon_StartStop(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr(),
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	time.Sleep(100 * time.Millisecond)

	// PID file should exist.
	if _, err := os.Stat(cfg.PIDFilePath); os.IsNotExist(err) {
		t.Error("PID file not created")
	}

	cancel()
	if err := <-done; err != nil {
		t.Errorf("Run returned error: %v", err)
	}

	// PID file should be cleaned up.
	if _, err := os.Stat(cfg.PIDFilePath); !os.IsNotExist(err) {
		t.Error("PID file not cleaned up")
	}
}

func TestDaemon_OnStartPartialFailure_RollsBack(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr() + "-rollback",
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	// 호출 순서를 기록할 슬라이스
	var callLog []string

	mod1 := &mockModule{
		name:    "mod1",
		onStart: func() { callLog = append(callLog, "mod1.start") },
		onStop:  func() { callLog = append(callLog, "mod1.stop") },
	}
	mod2 := &mockModule{
		name:    "mod2",
		onStart: func() { callLog = append(callLog, "mod2.start") },
		onStop:  func() { callLog = append(callLog, "mod2.stop") },
	}
	// mod3은 OnStart에서 에러를 반환
	mod3 := &failingMockModule{
		name:     "mod3",
		startErr: fmt.Errorf("mod3 start failed"),
		onStop:   func() { callLog = append(callLog, "mod3.stop") },
	}

	d.Register(mod1)
	d.Register(mod2)
	d.Register(mod3)

	ctx := context.Background()
	runErr := d.Run(ctx)

	// OnStart 실패 시 store가 닫히지 않으므로 테스트 cleanup을 위해 수동 Close
	d.Store().Close()

	// Run()이 에러를 반환해야 함
	if runErr == nil {
		t.Fatal("Run() should return error when module OnStart fails")
	}
	if !strings.Contains(runErr.Error(), "mod3") {
		t.Errorf("error should mention failing module name, got: %v", runErr)
	}

	// mod1, mod2의 OnStart가 호출됐는지 확인
	if !containsEntry(callLog, "mod1.start") {
		t.Error("mod1.OnStart should have been called")
	}
	if !containsEntry(callLog, "mod2.start") {
		t.Error("mod2.OnStart should have been called")
	}

	// 역순 롤백: mod2.stop → mod1.stop 순서로 호출돼야 함
	if !containsEntry(callLog, "mod2.stop") {
		t.Error("mod2.OnStop should have been called for rollback")
	}
	if !containsEntry(callLog, "mod1.stop") {
		t.Error("mod1.OnStop should have been called for rollback")
	}

	// mod3.OnStop은 호출되면 안 됨 (시작 안 됐으므로)
	if containsEntry(callLog, "mod3.stop") {
		t.Error("mod3.OnStop should NOT have been called (never started)")
	}

	// 역순 검증: callLog에서 mod2.stop이 mod1.stop보다 먼저 나와야 함
	mod2StopIdx := indexOf(callLog, "mod2.stop")
	mod1StopIdx := indexOf(callLog, "mod1.stop")
	if mod2StopIdx == -1 || mod1StopIdx == -1 {
		t.Fatal("stop entries missing from callLog")
	}
	if mod2StopIdx > mod1StopIdx {
		t.Errorf("rollback should be reverse order: mod2.stop (idx=%d) should come before mod1.stop (idx=%d)",
			mod2StopIdx, mod1StopIdx)
	}
}

// failingMockModule은 OnStart에서 에러를 반환하는 mock 모듈.
type failingMockModule struct {
	name     string
	startErr error
	onStop   func()
}

func (m *failingMockModule) Name() string                      { return m.name }
func (m *failingMockModule) RegisterRoutes(reg RouteRegistrar)  {}
func (m *failingMockModule) RegisterSchema(mig *store.Migrator) {}
func (m *failingMockModule) OnStart(ctx context.Context) error  { return m.startErr }
func (m *failingMockModule) OnStop() error {
	if m.onStop != nil {
		m.onStop()
	}
	return nil
}

func containsEntry(log []string, entry string) bool {
	for _, e := range log {
		if e == entry {
			return true
		}
	}
	return false
}

func indexOf(log []string, entry string) int {
	for i, e := range log {
		if e == entry {
			return i
		}
	}
	return -1
}

func TestDaemon_RegisterModule(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr() + "-mod",
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	startedCh := make(chan struct{}, 1)
	stoppedCh := make(chan struct{}, 1)

	d.Register(&mockModule{
		name:    "test",
		onStart: func() { startedCh <- struct{}{} },
		onStop:  func() { stoppedCh <- struct{}{} },
	})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	select {
	case <-startedCh:
	case <-time.After(5 * time.Second):
		t.Fatal("module OnStart not called within 5s")
	}

	cancel()
	<-done

	select {
	case <-stoppedCh:
	case <-time.After(5 * time.Second):
		t.Fatal("module OnStop not called within 5s")
	}
}

// mockModule for testing.
type mockModule struct {
	name    string
	onStart func()
	onStop  func()
}

func (m *mockModule) Name() string                      { return m.name }
func (m *mockModule) RegisterRoutes(reg RouteRegistrar)  {}
func (m *mockModule) RegisterSchema(mig *store.Migrator) {}
func (m *mockModule) OnStart(ctx context.Context) error {
	if m.onStart != nil {
		m.onStart()
	}
	return nil
}
func (m *mockModule) OnStop() error {
	if m.onStop != nil {
		m.onStop()
	}
	return nil
}
