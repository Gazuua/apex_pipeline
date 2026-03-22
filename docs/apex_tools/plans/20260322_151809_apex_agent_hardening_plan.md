# apex-agent 강화 구현 계획: Config + Install + Logging + E2E

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** apex-agent를 프로덕션 투입 가능한 수준으로 강화한다 — Config 시스템, 시스템 바이너리 설치, 로깅 시스템, E2E 통합 테스트.

**Architecture:** Config(TOML) → 시스템 설치(make install + post-merge hook + 버전 프로토콜) → 로깅(글로벌 slog + 패키지 래퍼 + 감사 로그) → E2E(격리된 TestEnv, 20개 시나리오) 순서대로 구축. 각 단계가 다음 단계의 기반.

**Tech Stack:** `github.com/BurntSushi/toml` (config), `gopkg.in/natefinch/lumberjack.v2` (로그 로테이션), `log/slog` (구조화 로깅)

**Spec:** `docs/apex_tools/plans/20260322_150859_apex_agent_hardening_spec.md`

**주의:** 모든 Go 소스 파일 첫 줄에 MIT 라이선스 헤더 필수:
`// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.`

---

## File Map

모든 경로는 `apex_tools/apex-agent/` 기준.

### 생성 파일

| 파일 | 책임 | Task |
|------|------|:----:|
| `internal/config/config.go` | Config 구조체, Load(), DefaultPath(), WriteDefault() | 1 |
| `internal/config/config_test.go` | Config 로드/기본값/부분오버라이드 테스트 | 1 |
| `internal/cli/config_cmd.go` | `config init`, `config show` CLI | 2 |
| `apex_tools/git-hooks/post-merge` | apex-agent 소스 변경 시 자동 빌드+설치 | 3 |
| `e2e/testenv/env.go` | E2E 테스트 환경 팩토리 (격리된 임시 환경) | 7 |
| `e2e/daemon_test.go` | E2E 그룹 A: 인프라 | 8 |
| `e2e/hook_test.go` | E2E 그룹 B: Hook 파이프라인 | 8 |
| `e2e/handoff_test.go` | E2E 그룹 C: 핸드오프 | 9 |
| `e2e/backlog_test.go` | E2E 그룹 D: 백로그 | 9 |
| `e2e/queue_test.go` | E2E 그룹 E: 큐 | 10 |
| `e2e/git_test.go` | E2E 그룹 F: Git 연동 | 10 |
| `e2e/session_test.go` | E2E 그룹 G: 세션 | 11 |
| `e2e/resilience_test.go` | E2E 그룹 H: 내성 | 11 |

### 수정 파일

| 파일 | 변경 내용 | Task |
|------|-----------|:----:|
| `internal/log/log.go` | 글로벌 래퍼 + WithModule + lumberjack 로테이션 + 감사 로그 | 5 |
| `internal/log/log_test.go` | 래퍼 함수 + 감사 태그 + 모듈 로거 테스트 | 5 |
| `cmd/apex-agent/main.go` | Version 변수 주입 포인트 | 3 |
| `internal/cli/root.go` | config 커맨드 추가 | 2 |
| `internal/cli/daemon_cmd.go` | config.Load() 사용 + 로거 초기화 | 2, 6 |
| `internal/daemon/daemon.go` | log.New() → log.WithModule() 전환 | 6 |
| `internal/ipc/client.go` | 버전 체크 + 자동 데몬 재시작 | 4 |
| `internal/ipc/server.go` | 로그 추가 | 6 |
| `internal/daemon/router.go` | 로그 추가 | 6 |
| `internal/modules/handoff/manager.go` | 로그 추가 | 6 |
| `internal/modules/handoff/gate.go` | 로그 추가 | 6 |
| `internal/modules/backlog/manage.go` | 로그 추가 | 6 |
| `internal/modules/queue/manager.go` | 로그 추가 | 6 |
| `internal/modules/hook/gate.go` | 로그 추가 | 6 |
| `internal/cleanup/cleanup.go` | 로그 추가 | 6 |
| `internal/context/context.go` | 로그 추가 | 6 |
| `internal/plugin/setup.go` | 로그 추가 | 6 |
| `internal/platform/env.go` | APEX_* 환경변수 참조 제거 (config로 대체) | 2 |
| `Makefile` | install 타겟 추가 | 3 |
| `.claude/settings.json` | hook 경로를 시스템 PATH 기반으로 변경 | 3 |
| `e2e/e2e_test.go` | `//go:build e2e` 태그 제거 | 7 |

### 삭제 파일

없음 (이번 강화는 기존 파일 수정만).

---

## 의존성 그래프

```
Task 1 (Config 패키지)
  └→ Task 2 (Config CLI + daemon 통합)
       └→ Task 3 (시스템 설치 + 버전 주입)
            └→ Task 4 (버전 프로토콜)
Task 5 (로깅 시스템 확장)
  └→ Task 6 (전 모듈 로그 추가)
       └→ Task 7 (E2E TestEnv)
            ├→ Task 8 (E2E A+B: 인프라+Hook)
            ├→ Task 9 (E2E C+D: 핸드오프+백로그)
            ├→ Task 10 (E2E E+F: 큐+Git)
            └→ Task 11 (E2E G+H: 세션+내성)
```

Tasks 1~4 (Config/Install)와 Task 5 (Logging)는 독립 — 병렬 가능.
Task 6 (로그 추가)는 Task 2 + Task 5 모두 필요.
Tasks 8~11 (E2E 시나리오)은 Task 7 완료 후 병렬 가능.

---

## Task 1: Config 패키지

**Files:**
- Create: `internal/config/config.go`
- Create: `internal/config/config_test.go`

**Ref:** 스펙 §3 Config 시스템

- [ ] **Step 1: config 테스트 작성**

```go
// internal/config/config_test.go
package config

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestDefaults(t *testing.T) {
	cfg := Defaults()
	if cfg.Daemon.IdleTimeout != 30*time.Minute {
		t.Errorf("IdleTimeout = %v, want 30m", cfg.Daemon.IdleTimeout)
	}
	if cfg.Log.Level != "debug" {
		t.Errorf("Log.Level = %q, want 'debug'", cfg.Log.Level)
	}
	if !cfg.Log.Audit {
		t.Error("Log.Audit should default to true")
	}
}

func TestLoad_FileNotFound_ReturnsDefaults(t *testing.T) {
	cfg, err := Load("/nonexistent/config.toml")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Daemon.IdleTimeout != 30*time.Minute {
		t.Error("missing file should return defaults")
	}
}

func TestLoad_PartialOverride(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	os.WriteFile(path, []byte(`
[daemon]
idle_timeout = "10m"

[log]
level = "warn"
`), 0o644)

	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Daemon.IdleTimeout != 10*time.Minute {
		t.Errorf("IdleTimeout = %v, want 10m", cfg.Daemon.IdleTimeout)
	}
	if cfg.Log.Level != "warn" {
		t.Errorf("Log.Level = %q, want 'warn'", cfg.Log.Level)
	}
	// Non-overridden fields keep defaults
	if !cfg.Log.Audit {
		t.Error("Log.Audit should remain default true")
	}
}

func TestLoad_FullConfig(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	os.WriteFile(path, []byte(`
[daemon]
idle_timeout = "1h"
socket_path = "/tmp/test.sock"

[store]
db_path = "/tmp/test.db"

[queue]
stale_timeout = "2h"
poll_interval = "500ms"

[log]
level = "error"
file = "test.log"
max_size_mb = 100
max_backups = 5
audit = false

[build]
command = "make build"
presets = ["debug", "release", "asan"]
`), 0o644)

	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Store.DBPath != "/tmp/test.db" {
		t.Errorf("DBPath = %q", cfg.Store.DBPath)
	}
	if cfg.Queue.StaleTimeout != 2*time.Hour {
		t.Errorf("StaleTimeout = %v", cfg.Queue.StaleTimeout)
	}
	if len(cfg.Build.Presets) != 3 {
		t.Errorf("Presets = %v", cfg.Build.Presets)
	}
}

func TestWriteDefault_CreatesFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.toml")
	if err := WriteDefault(path); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	if len(data) == 0 {
		t.Error("WriteDefault should create non-empty file")
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `go test ./internal/config/... -v`
Expected: FAIL — 패키지 미존재

- [ ] **Step 3: config.go 구현**

```go
// internal/config/config.go
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"time"

	"github.com/BurntSushi/toml"
)

// Duration wraps time.Duration for TOML unmarshaling.
type Duration time.Duration

func (d *Duration) UnmarshalText(text []byte) error {
	dur, err := time.ParseDuration(string(text))
	if err != nil {
		return err
	}
	*d = Duration(dur)
	return nil
}

type Config struct {
	Daemon DaemonConfig `toml:"daemon"`
	Store  StoreConfig  `toml:"store"`
	Queue  QueueConfig  `toml:"queue"`
	Log    LogConfig    `toml:"log"`
	Build  BuildConfig  `toml:"build"`
}

type DaemonConfig struct {
	IdleTimeout time.Duration // populated from rawIdleTimeout
	SocketPath  string        `toml:"socket_path"`

	RawIdleTimeout Duration `toml:"idle_timeout"`
}

type StoreConfig struct {
	DBPath string `toml:"db_path"`
}

type QueueConfig struct {
	StaleTimeout time.Duration
	PollInterval time.Duration

	RawStaleTimeout Duration `toml:"stale_timeout"`
	RawPollInterval Duration `toml:"poll_interval"`
}

type LogConfig struct {
	Level      string `toml:"level"`
	File       string `toml:"file"`
	MaxSizeMB  int    `toml:"max_size_mb"`
	MaxBackups int    `toml:"max_backups"`
	Audit      bool   `toml:"audit"`
}

type BuildConfig struct {
	Command string   `toml:"command"`
	Presets []string `toml:"presets"`
}

// Defaults returns a Config with all default values.
func Defaults() *Config {
	return &Config{
		Daemon: DaemonConfig{
			IdleTimeout: 30 * time.Minute,
		},
		Store: StoreConfig{},
		Queue: QueueConfig{
			StaleTimeout: 1 * time.Hour,
			PollInterval: 1 * time.Second,
		},
		Log: LogConfig{
			Level:      "debug",
			File:       "apex-agent.log",
			MaxSizeMB:  50,
			MaxBackups: 3,
			Audit:      true,
		},
		Build: BuildConfig{
			Command: buildCommand(),
			Presets: []string{"debug", "release"},
		},
	}
}

// Load reads config from path. If file doesn't exist, returns defaults.
func Load(path string) (*Config, error) {
	cfg := Defaults()

	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return nil, fmt.Errorf("read config: %w", err)
	}

	if _, err := toml.Decode(string(data), cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	// Convert Duration fields to time.Duration
	if cfg.Daemon.RawIdleTimeout != 0 {
		cfg.Daemon.IdleTimeout = time.Duration(cfg.Daemon.RawIdleTimeout)
	}
	if cfg.Queue.RawStaleTimeout != 0 {
		cfg.Queue.StaleTimeout = time.Duration(cfg.Queue.RawStaleTimeout)
	}
	if cfg.Queue.RawPollInterval != 0 {
		cfg.Queue.PollInterval = time.Duration(cfg.Queue.RawPollInterval)
	}

	return cfg, nil
}

// DefaultPath returns the platform-specific config file path.
func DefaultPath() string {
	if runtime.GOOS == "windows" {
		return filepath.Join(os.Getenv("LOCALAPPDATA"), "apex-agent", "config.toml")
	}
	if xdg := os.Getenv("XDG_CONFIG_HOME"); xdg != "" {
		return filepath.Join(xdg, "apex-agent", "config.toml")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".config", "apex-agent", "config.toml")
}

// WriteDefault creates a config.toml with documented defaults.
func WriteDefault(path string) error {
	os.MkdirAll(filepath.Dir(path), 0o755)
	content := `# apex-agent configuration
# 값을 비우면 플랫폼 기본값 사용. 이 파일이 없어도 전부 기본값으로 동작.

[daemon]
idle_timeout = "30m"
# socket_path = ""          # Named Pipe (Win) or Unix Socket (Linux)

[store]
# db_path = ""              # SQLite 경로

[queue]
stale_timeout = "1h"
poll_interval = "1s"

[log]
level = "debug"             # debug | info | warn | error
file = "apex-agent.log"     # 데이터 디렉토리 기준 상대 경로
max_size_mb = 50
max_backups = 3
audit = true                # 감사 로그 (lock, 상태 전이 등)

[build]
command = "cmd.exe /c build.bat"
presets = ["debug", "release"]
`
	return os.WriteFile(path, []byte(content), 0o644)
}

func buildCommand() string {
	if runtime.GOOS == "windows" {
		return "cmd.exe /c build.bat"
	}
	return "./build.sh"
}
```

**참고:** Duration 타입의 TOML 파싱은 `UnmarshalText`로 `"30m"`, `"1h"` 등의 문자열을 `time.Duration`으로 변환. `RawIdleTimeout` 등의 중간 필드는 TOML 디코딩용이고, 실제 사용은 `IdleTimeout` (time.Duration) 필드.

- [ ] **Step 4: 의존성 추가**

```bash
cd apex_tools/apex-agent && go get github.com/BurntSushi/toml && go mod tidy
```

- [ ] **Step 5: 테스트 통과 확인**

Run: `go test ./internal/config/... -v`
Expected: PASS

- [ ] **Step 6: 커밋**

```bash
git add internal/config/ go.mod go.sum
git commit -m "feat(tools): BACKLOG-126 Config 패키지 — TOML 설정 로드 + 기본값"
git push
```

---

## Task 2: Config CLI + daemon 통합

**Files:**
- Create: `internal/cli/config_cmd.go`
- Modify: `internal/cli/root.go` — config 커맨드 추가
- Modify: `internal/cli/daemon_cmd.go` — config.Load() 사용
- Modify: `internal/platform/env.go` — APEX_* 환경변수 제거 (필요 시)

- [ ] **Step 1: config_cmd.go 작성**

```go
// internal/cli/config_cmd.go
package cli

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
)

func configCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "config",
		Short: "설정 관리",
	}
	cmd.AddCommand(configInitCmd())
	cmd.AddCommand(configShowCmd())
	return cmd
}

func configInitCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "init",
		Short: "기본 config.toml 생성",
		RunE: func(cmd *cobra.Command, args []string) error {
			path := config.DefaultPath()
			if err := config.WriteDefault(path); err != nil {
				return err
			}
			fmt.Printf("config written to %s\n", path)
			return nil
		},
	}
}

func configShowCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "show",
		Short: "현재 설정 출력",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, err := config.Load(config.DefaultPath())
			if err != nil {
				return err
			}
			fmt.Printf("idle_timeout: %s\n", cfg.Daemon.IdleTimeout)
			fmt.Printf("socket_path: %s\n", cfg.Daemon.SocketPath)
			fmt.Printf("db_path: %s\n", cfg.Store.DBPath)
			fmt.Printf("log.level: %s\n", cfg.Log.Level)
			fmt.Printf("log.file: %s\n", cfg.Log.File)
			fmt.Printf("log.audit: %v\n", cfg.Log.Audit)
			return nil
		},
	}
}
```

- [ ] **Step 2: root.go에 config 커맨드 등록**

`root.AddCommand(configCmd())` 추가.

- [ ] **Step 3: daemon_cmd.go에서 config.Load() 사용**

`daemonRunCmd` 수정:
```go
// 기존: platform 함수로 직접 구성
// 변경: config.Load() → daemon.Config 변환

cfg, err := config.Load(config.DefaultPath())
if err != nil {
    return err
}

daemonCfg := daemon.Config{
    DBPath:      cfg.Store.DBPath,
    PIDFilePath: platform.PIDFilePath(),
    SocketAddr:  cfg.Daemon.SocketPath,
    IdleTimeout: cfg.Daemon.IdleTimeout,
}
// 빈 값은 플랫폼 기본값으로 대체
if daemonCfg.DBPath == "" {
    daemonCfg.DBPath = platform.DBPath()
}
if daemonCfg.SocketAddr == "" {
    daemonCfg.SocketAddr = platform.SocketPath()
}
```

- [ ] **Step 4: 빌드 + 수동 테스트**

```bash
go build ./cmd/apex-agent
./apex-agent config init    # config.toml 생성
./apex-agent config show    # 설정 출력
./apex-agent daemon start   # config 기반 데몬 시작
./apex-agent daemon stop
```

- [ ] **Step 5: 전체 테스트 + 커밋**

Run: `go test ./... -v`
Expected: PASS

```bash
git add internal/cli/config_cmd.go internal/cli/root.go internal/cli/daemon_cmd.go internal/platform/env.go
git commit -m "feat(tools): BACKLOG-126 Config CLI + daemon 통합"
git push
```

---

## Task 3: 시스템 바이너리 설치 + 버전 주입

**Files:**
- Modify: `Makefile` — install 타겟 추가
- Modify: `cmd/apex-agent/main.go` — Version 변수
- Create: `apex_tools/git-hooks/post-merge` (프로젝트 루트 기준)
- Modify: `.claude/settings.json` — PATH 기반 호출로 변경

- [ ] **Step 1: main.go에 Version 변수 추가**

```go
package main

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/cli"

var Version = "dev"

func main() {
	cli.Version = Version
	cli.Execute()
}
```

- [ ] **Step 2: Makefile에 install 타겟 추가**

```makefile
VERSION = $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")

ifeq ($(OS),Windows_NT)
    INSTALL_DIR ?= $(LOCALAPPDATA)/apex-agent
    EXE = .exe
else
    INSTALL_DIR ?= $(HOME)/.local/bin
    EXE =
endif

install:
	@mkdir -p $(INSTALL_DIR)
	go build -ldflags="-X main.Version=$(VERSION)" $(GOFLAGS) -o $(INSTALL_DIR)/apex-agent$(EXE) ./cmd/apex-agent
	@echo "installed: $(INSTALL_DIR)/apex-agent$(EXE) ($(VERSION))"
```

기존 `build` 타겟의 `GOFLAGS`에도 Version 주입 추가.

- [ ] **Step 3: post-merge git hook 작성**

```bash
#!/usr/bin/env bash
# apex_tools/git-hooks/post-merge
# apex-agent 소스 변경 감지 시 자동 빌드+설치

changed=$(git diff HEAD@{1} --name-only 2>/dev/null | grep "apex_tools/apex-agent/" || true)
if [ -n "$changed" ]; then
    echo "[apex-agent] 소스 변경 감지, 자동 빌드+설치 중..."
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
    cd "$PROJECT_ROOT/apex_tools/apex-agent" && make install
fi
```

`chmod +x apex_tools/git-hooks/post-merge`

- [ ] **Step 4: settings.json 경로 변경**

모든 hook command를 시스템 PATH 기반으로:
```json
"command": "apex-agent hook validate-build"
```
(`./apex_tools/apex-agent/apex-agent` → `apex-agent`)

**참고:** Windows에서 `%LOCALAPPDATA%/apex-agent/`가 PATH에 있어야 함. `make install` 후 사용자에게 PATH 추가 안내 출력.

- [ ] **Step 5: 커밋**

```bash
git add Makefile cmd/apex-agent/main.go apex_tools/git-hooks/post-merge .claude/settings.json
git commit -m "feat(tools): BACKLOG-126 시스템 설치 + 버전 주입 + post-merge 자동 빌드"
git push
```

---

## Task 4: 버전 프로토콜

**Files:**
- Modify: `internal/ipc/client.go` — 버전 체크 + 자동 재시작
- Modify: `internal/daemon/daemon.go` — version 핸들러 등록
- Modify: `internal/cli/root.go` — Version 변수 export

- [ ] **Step 1: daemon에 version 핸들러 추가**

`daemon.go`의 `Run()` 에서 내장 라우트 등록:
```go
d.router.RegisterModule("daemon", func(reg RouteRegistrar) {
    reg.Handle("version", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
        return map[string]string{"version": cli.Version}, nil
    })
})
```

또는 별도 `version.go` 파일에 버전을 보관.

- [ ] **Step 2: client.go에 버전 체크 추가**

```go
// Send 전에 버전 체크 (첫 호출 시 1회)
func (c *Client) ensureVersion() error {
    resp, err := c.sendRaw("daemon", "version", nil, "")
    if err != nil {
        return nil // 데몬 없음 → 정상 (auto-start에서 처리)
    }
    var data map[string]string
    json.Unmarshal(resp.Data, &data)
    if data["version"] != Version && Version != "dev" {
        // 버전 불일치 → 데몬 재시작
        c.sendRaw("daemon", "shutdown", nil, "")
        time.Sleep(500 * time.Millisecond)
        // auto-start가 새 버전으로 시작
    }
    return nil
}
```

`Version` 변수는 `cli.Version`에서 가져오거나 패키지 레벨 변수로 설정.

- [ ] **Step 3: 테스트**

기존 daemon_test.go에 버전 핸들러 테스트 추가:
```go
func TestDaemon_VersionHandler(t *testing.T) {
    // 데몬 시작 → version 요청 → 버전 문자열 반환
}
```

- [ ] **Step 4: 커밋**

```bash
git add internal/ipc/client.go internal/daemon/daemon.go internal/cli/root.go
git commit -m "feat(tools): BACKLOG-126 버전 프로토콜 — 불일치 시 데몬 자동 재시작"
git push
```

---

## Task 5: 로깅 시스템 확장

**Files:**
- Modify: `internal/log/log.go` — 전면 재작성 (글로벌 래퍼 + WithModule + lumberjack)
- Modify: `internal/log/log_test.go` — 새 API 테스트

**Ref:** 스펙 §5 로깅 시스템

- [ ] **Step 1: log_test.go 재작성**

```go
package log

import (
	"bytes"
	"strings"
	"testing"
)

func TestDebug_WritesToBuffer(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "debug", Writer: &buf})
	Debug("test message", "key", "value")

	out := buf.String()
	if !strings.Contains(out, "test message") {
		t.Errorf("output = %q, want contains 'test message'", out)
	}
}

func TestAudit_HasAuditTag(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "debug", Writer: &buf})
	Audit("lock acquired", "channel", "build")

	out := buf.String()
	if !strings.Contains(out, "audit=true") {
		t.Errorf("audit log missing audit=true tag: %q", out)
	}
}

func TestWithModule_AddsModuleField(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "debug", Writer: &buf})
	ml := WithModule("handoff")
	ml.Info("state changed")

	out := buf.String()
	if !strings.Contains(out, "module=handoff") {
		t.Errorf("module log missing module field: %q", out)
	}
}

func TestLevelFilter_InfoHidesDebug(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "info", Writer: &buf})
	Debug("should be hidden")
	Info("should be visible")

	out := buf.String()
	if strings.Contains(out, "should be hidden") {
		t.Error("debug message should be filtered at info level")
	}
	if !strings.Contains(out, "should be visible") {
		t.Error("info message should be visible")
	}
}
```

- [ ] **Step 2: log.go 재작성**

```go
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
```

**참고:** `Init()`에서 lumberjack를 사용한 파일 출력 + `io.MultiWriter`로 stderr+파일 듀얼 출력은 daemon_cmd.go에서 설정:

```go
// daemon_cmd.go에서 (Task 6)
import "gopkg.in/natefinch/lumberjack.v2"

fileWriter := &lumberjack.Logger{
    Filename:   logFilePath,
    MaxSize:    cfg.Log.MaxSizeMB,
    MaxBackups: cfg.Log.MaxBackups,
}
w := io.MultiWriter(os.Stderr, fileWriter)
log.Init(log.LogConfig{Level: cfg.Log.Level, Writer: w})
```

- [ ] **Step 3: 의존성 추가**

```bash
go get gopkg.in/natefinch/lumberjack.v2 && go mod tidy
```

- [ ] **Step 4: 테스트 + 커밋**

Run: `go test ./internal/log/... -v`
Expected: PASS

```bash
git add internal/log/ go.mod go.sum
git commit -m "feat(tools): BACKLOG-126 로깅 시스템 — 글로벌 래퍼 + WithModule + 감사 로그"
git push
```

---

## Task 6: 전 모듈 로그 추가

**Files:** 수정 대상 11개 파일 (File Map 참조)

**원칙:**
- 각 모듈 파일 상단에 `var ml = log.WithModule("모듈명")` 선언
- 모든 핸들러 진입: `ml.Debug("action", "param", value)`
- 상태 변경: `ml.Info(...)` 또는 `ml.Audit(...)`
- 에러: `ml.Error("msg", "err", err)`
- daemon_cmd.go에서 `log.Init()` 호출 추가 (lumberjack 연결)

- [ ] **Step 1: daemon.go + daemon_cmd.go 로그 통합**

daemon.go: `log.New()` 인스턴스 제거 → `log.WithModule("daemon")` 사용
daemon_cmd.go: config 기반 `log.Init()` 호출 + lumberjack 설정

- [ ] **Step 2: ipc/server.go + router.go 로그 추가**

```go
var ml = log.WithModule("ipc")
// handleConn: ml.Debug("request received", "module", req.Module, "action", req.Action)
// handleConn: ml.Debug("response sent", "ok", resp.OK)
```

```go
var ml = log.WithModule("router")
// Dispatch: ml.Debug("dispatching", "module", module, "action", action)
```

- [ ] **Step 3: handoff 모듈 로그 추가**

```go
var ml = log.WithModule("handoff")
// NotifyStart: ml.Audit("branch registered", "branch", branch, "backlog_id", backlogID)
// NotifyTransition: ml.Audit("state transition", "branch", branch, "from", current, "to", next)
// ValidateCommit: ml.Debug("commit gate", "branch", branch, "allowed", err==nil)
// ValidateEdit: ml.Debug("edit gate", "branch", branch, "file", filePath, "allowed", err==nil)
```

- [ ] **Step 4: backlog, queue, hook 모듈 로그 추가**

각 모듈 동일 패턴:
```go
var ml = log.WithModule("backlog")
var ml = log.WithModule("queue")
var ml = log.WithModule("hook")
```

주요 이벤트에 Debug/Info/Audit 로그 삽입.

- [ ] **Step 5: 전체 테스트 + 커밋**

Run: `go test ./... -v`
Expected: PASS (로그 추가는 기능 변경 아님)

```bash
git add internal/
git commit -m "feat(tools): BACKLOG-126 전 모듈 구조화 로깅 추가"
git push
```

---

## Task 7: E2E TestEnv 인프라

**Files:**
- Create: `e2e/testenv/env.go`
- Modify: `e2e/e2e_test.go` — `//go:build e2e` 태그 제거, TestEnv 사용

- [ ] **Step 1: testenv/env.go 작성**

```go
// e2e/testenv/env.go
package testenv

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/handoff"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/queue"
)

type TestEnv struct {
	Dir        string
	ConfigPath string
	DBPath     string
	SocketAddr string
	Client     *ipc.Client
	Cancel     context.CancelFunc

	daemon *daemon.Daemon
	done   chan error
}

// New creates a fully isolated test environment with daemon running.
func New(t *testing.T) *TestEnv {
	t.Helper()

	dir := t.TempDir()
	dbPath := filepath.Join(dir, "test.db")
	socketAddr := testSocketAddr(t.Name())

	// Initialize logger to discard (avoid test output noise)
	log.Init(log.LogConfig{Level: "debug", Writer: os.Stderr})

	cfg := daemon.Config{
		DBPath:      dbPath,
		PIDFilePath: filepath.Join(dir, "test.pid"),
		SocketAddr:  socketAddr,
		IdleTimeout: 5 * time.Minute,
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatalf("daemon.New: %v", err)
	}

	// Register all modules
	d.Register(hook.New())
	d.Register(backlog.New(d.Store()))
	d.Register(handoff.New(d.Store()))
	d.Register(queue.New(d.Store()))

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	// Wait for readiness
	time.Sleep(100 * time.Millisecond)

	client := ipc.NewClient(socketAddr)

	env := &TestEnv{
		Dir:        dir,
		ConfigPath: filepath.Join(dir, "config.toml"),
		DBPath:     dbPath,
		SocketAddr: socketAddr,
		Client:     client,
		Cancel:     cancel,
		daemon:     d,
		done:       done,
	}

	t.Cleanup(func() {
		cancel()
		<-done
	})

	return env
}

// Stop stops the daemon. Use for tests that need daemon restart.
func (e *TestEnv) Stop() {
	e.Cancel()
	<-e.done
}

// Restart starts a new daemon on the same environment.
func (e *TestEnv) Restart(t *testing.T) {
	t.Helper()

	cfg := daemon.Config{
		DBPath:      e.DBPath,
		PIDFilePath: filepath.Join(e.Dir, "test.pid"),
		SocketAddr:  e.SocketAddr,
		IdleTimeout: 5 * time.Minute,
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatalf("daemon.New on restart: %v", err)
	}
	d.Register(hook.New())
	d.Register(backlog.New(d.Store()))
	d.Register(handoff.New(d.Store()))
	d.Register(queue.New(d.Store()))

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()
	time.Sleep(100 * time.Millisecond)

	e.Cancel = cancel
	e.daemon = d
	e.done = done
	e.Client = ipc.NewClient(e.SocketAddr)

	t.Cleanup(func() {
		cancel()
		<-done
	})
}

// InitGitRepo creates a temporary git repository for git-related tests.
func (e *TestEnv) InitGitRepo(t *testing.T) string {
	t.Helper()
	repoDir := filepath.Join(e.Dir, "repo")
	os.MkdirAll(repoDir, 0o755)

	// git init + initial commit
	run(t, repoDir, "git", "init")
	run(t, repoDir, "git", "config", "user.email", "test@test.com")
	run(t, repoDir, "git", "config", "user.name", "Test")
	os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("# test\n"), 0o644)
	run(t, repoDir, "git", "add", ".")
	run(t, repoDir, "git", "commit", "-m", "initial commit")

	return repoDir
}

func testSocketAddr(name string) string {
	// Generate unique socket address per test
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-e2e-` + sanitize(name)
	}
	return "/tmp/apex-agent-e2e-" + sanitize(name) + ".sock"
}

func sanitize(name string) string {
	// Replace non-alphanumeric characters for safe pipe/socket names
	result := make([]byte, 0, len(name))
	for _, b := range []byte(name) {
		if (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') {
			result = append(result, b)
		} else {
			result = append(result, '_')
		}
	}
	return string(result)
}

func run(t *testing.T, dir string, cmd string, args ...string) {
	t.Helper()
	c := exec.Command(cmd, args...)
	c.Dir = dir
	if out, err := c.CombinedOutput(); err != nil {
		t.Fatalf("%s %v failed: %v\n%s", cmd, args, err, out)
	}
}
```

- [ ] **Step 2: 기존 e2e_test.go의 `//go:build e2e` 태그 제거 + TestEnv 사용으로 리팩터링**

- [ ] **Step 3: 테스트 + 커밋**

Run: `go test ./e2e/... -v`
Expected: PASS (기존 E2E + TestEnv 기반)

```bash
git add e2e/
git commit -m "feat(tools): BACKLOG-126 E2E TestEnv 인프라 — 격리된 테스트 환경"
git push
```

---

## Task 8: E2E 그룹 A+B (인프라 + Hook)

**Files:**
- Create: `e2e/daemon_test.go`
- Create: `e2e/hook_test.go`

**시나리오 #1, #2, #3, #4:**

- [ ] **Step 1: daemon_test.go 작성**

```go
// #1: 데몬 시작 → IPC 왕복 → 응답 확인
func TestDaemon_StartIPCRoundtripIdleShutdown(t *testing.T)
```

- [ ] **Step 2: hook_test.go 작성**

```go
// #2: validate-build 차단/허용
func TestHook_ValidateBuildBlocksAndAllows(t *testing.T)

// #3: validate-merge 잠금 필요
func TestHook_ValidateMergeRequiresLock(t *testing.T)

// #4: 비정상 입력 내성
func TestHook_MalformedInput(t *testing.T)
```

- [ ] **Step 3: 테스트 + 커밋**

```bash
git add e2e/daemon_test.go e2e/hook_test.go
git commit -m "test(tools): BACKLOG-126 E2E 그룹 A+B — 인프라 + Hook 파이프라인"
git push
```

---

## Task 9: E2E 그룹 C+D (핸드오프 + 백로그)

**Files:**
- Create: `e2e/handoff_test.go`
- Create: `e2e/backlog_test.go`

**시나리오 #5, #6, #7, #8, #9, #10:**

- [ ] **Step 1: handoff_test.go 작성**

```go
// #5: 전체 상태머신 사이클
func TestHandoff_FullLifecycle(t *testing.T)

// #6: 게이트 강제
func TestHandoff_GateEnforcement(t *testing.T)

// #7: 멀티 워크스페이스
func TestHandoff_MultiWorkspace(t *testing.T)
```

- [ ] **Step 2: backlog_test.go 작성**

```go
// #8: CRUD + export
func TestBacklog_CRUDAndExport(t *testing.T)

// #9: 마이그레이션 라운드트립
func TestBacklog_MigrationRoundtrip(t *testing.T)

// #10: 라운드트립 정합성 (diff 0)
func TestBacklog_RoundtripFidelity(t *testing.T)
```

- [ ] **Step 3: 테스트 + 커밋**

```bash
git add e2e/handoff_test.go e2e/backlog_test.go
git commit -m "test(tools): BACKLOG-126 E2E 그룹 C+D — 핸드오프 + 백로그"
git push
```

---

## Task 10: E2E 그룹 E+F (큐 + Git)

**Files:**
- Create: `e2e/queue_test.go`
- Create: `e2e/git_test.go`

**시나리오 #11, #12, #13, #14:**

- [ ] **Step 1: queue_test.go 작성**

```go
// #11: acquire → release → 재acquire
func TestQueue_AcquireReleaseSerialize(t *testing.T)

// #12: 동시 부하 (-short 시 스킵)
func TestQueue_ConcurrentStress(t *testing.T)
```

- [ ] **Step 2: git_test.go 작성**

```go
// #13: 자동 rebase + 충돌 차단
func TestEnforceRebase_AutoAndConflict(t *testing.T)

// #14: 머지된 브랜치 감지
func TestCleanup_MergedBranchDetection(t *testing.T)
```

`InitGitRepo()`로 임시 레포 생성 후 테스트.

- [ ] **Step 3: 테스트 + 커밋**

```bash
git add e2e/queue_test.go e2e/git_test.go
git commit -m "test(tools): BACKLOG-126 E2E 그룹 E+F — 큐 + Git 연동"
git push
```

---

## Task 11: E2E 그룹 G+H (세션 + 내성)

**Files:**
- Create: `e2e/session_test.go`
- Create: `e2e/resilience_test.go`

**시나리오 #15, #16, #17, #18, #19, #20:**

- [ ] **Step 1: session_test.go 작성**

```go
// #15: 컨텍스트 출력 포맷
func TestContext_OutputFormat(t *testing.T)

// #16: 플러그인 idempotent 설정
func TestPlugin_IdempotentSetup(t *testing.T)
```

- [ ] **Step 2: resilience_test.go 작성**

```go
// #17: 잘못된 IPC → 크래시 없음
func TestResilience_MalformedIPC(t *testing.T)

// #18: DB 삭제 → 재시작 → 자동 재생성
func TestResilience_DBAutoRecreate(t *testing.T)

// #19: stale PID → 자동 복구
func TestResilience_StalePIDRecovery(t *testing.T)

// #20: 커스텀 config 경로
func TestResilience_CustomConfigPaths(t *testing.T)
```

#18: `env.Stop()` → DB 파일 삭제 → `env.Restart()` → 데몬 정상 동작 확인
#19: PID 파일에 죽은 PID 기록 → 새 데몬 시작 시도 → stale 감지 → 정상 시작

- [ ] **Step 3: 전체 E2E + 전체 테스트 + 커밋**

Run: `go test ./e2e/... -v` → 20개 시나리오 PASS
Run: `go test ./... -v` → 전체 PASS

```bash
git add e2e/
git commit -m "test(tools): BACKLOG-126 E2E 그룹 G+H — 세션 + 내성 (20개 시나리오 완성)"
git push
```

---

## 완료 기준

1. `go test ./... -v` — 단위 + E2E 전부 PASS
2. `apex-agent config init` → config.toml 생성
3. `make install` → 시스템 바이너리 설치
4. `apex-agent version` → 빌드 시 주입된 버전 표시
5. 데몬 로그에 DEBUG + AUDIT 출력 확인
6. E2E 20개 시나리오 전부 PASS
7. settings.json이 시스템 PATH 기반 호출
8. post-merge hook이 소스 변경 감지 시 자동 빌드
