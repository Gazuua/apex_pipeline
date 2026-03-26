// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package config

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
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
	HTTP   HTTPConfig   `toml:"http"`
}

type HTTPConfig struct {
	Enabled bool   `toml:"enabled"`
	Addr    string `toml:"addr"`
}

type DaemonConfig struct {
	SocketPath string `toml:"socket_path"`
}

type StoreConfig struct {
	DBPath string `toml:"db_path"`
}

type QueueConfig struct {
	StaleTimeout time.Duration
	PollInterval time.Duration

	// RawStaleTimeout and RawPollInterval are exported only because the TOML
	// decoder requires exported fields. They are internal to Load() — callers
	// should use StaleTimeout and PollInterval instead.
	RawStaleTimeout Duration `toml:"stale_timeout"`
	RawPollInterval Duration `toml:"poll_interval"`
}

type LogConfig struct {
	Level      string `toml:"level"`
	File       string `toml:"file"`           // deprecated: 하위 호환용 유지. 로그 디렉토리 결정에만 사용
	MaxSizeMB  int    `toml:"max_size_mb"`    // deprecated: daily rotation으로 대체
	MaxBackups int    `toml:"max_backups"`    // deprecated: max_days로 대체
	MaxDays    int    `toml:"max_days"`       // 일별 로그 보존 일수 (0 = 무제한)
	Audit      bool   `toml:"audit"`
}

type BuildConfig struct {
	Command string   `toml:"command"`
	Presets []string `toml:"presets"`
}

// Defaults returns a Config with all default values.
func Defaults() *Config {
	return &Config{
		Daemon: DaemonConfig{},
		Store: StoreConfig{},
		Queue: QueueConfig{
			StaleTimeout: 1 * time.Hour,
			PollInterval: 1 * time.Second,
		},
		Log: LogConfig{
			Level:      "info",
			File:       "apex-agent.log",
			MaxSizeMB:  50,
			MaxBackups: 3,
			MaxDays:    30,
			Audit:      true,
		},
		Build: BuildConfig{
			Command: buildCommand(),
			Presets: []string{"debug", "release"},
		},
		HTTP: HTTPConfig{
			Enabled: true,
			Addr:    "localhost:7600",
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
	if cfg.Queue.RawStaleTimeout != 0 {
		cfg.Queue.StaleTimeout = time.Duration(cfg.Queue.RawStaleTimeout)
	}
	if cfg.Queue.RawPollInterval != 0 {
		cfg.Queue.PollInterval = time.Duration(cfg.Queue.RawPollInterval)
	}

	// Warn if HTTP addr is not localhost-bound (no authentication).
	if cfg.HTTP.Enabled && cfg.HTTP.Addr != "" {
		host := cfg.HTTP.Addr
		if idx := strings.LastIndex(host, ":"); idx >= 0 {
			host = host[:idx]
		}
		if host != "localhost" && host != "127.0.0.1" && host != "::1" && host != "" {
			fmt.Fprintf(os.Stderr, "WARNING: HTTP 대시보드가 %q에 바인딩됩니다. 인증이 없으므로 localhost 외 주소는 보안 위험이 있습니다.\n", cfg.HTTP.Addr)
		}
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
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}
	content := `# apex-agent configuration
# 값을 비우면 플랫폼 기본값 사용. 이 파일이 없어도 전부 기본값으로 동작.

[daemon]
# socket_path = ""          # Named Pipe (Win) or Unix Socket (Linux)

[store]
# db_path = ""              # SQLite 경로

[queue]
stale_timeout = "1h"
poll_interval = "1s"

[log]
level = "info"              # debug | info | warn | error
# file = "apex-agent.log"  # deprecated: 일별 로그로 대체됨
max_days = 30               # 일별 로그 보존 일수 (0 = 무제한). logs/YYYYMMDD.log 형식
audit = true                # 감사 로그 (lock, 상태 전이 등)

[build]
command = "cmd.exe /c build.bat"
presets = ["debug", "release"]

[http]
enabled = true
addr = "localhost:7600"     # 대시보드 바인딩 주소
`
	return os.WriteFile(path, []byte(content), 0o644)
}

func buildCommand() string {
	if runtime.GOOS == "windows" {
		return "cmd.exe /c build.bat"
	}
	return "./build.sh"
}
