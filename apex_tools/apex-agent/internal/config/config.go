// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

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
	HTTP   HTTPConfig   `toml:"http"`
}

type HTTPConfig struct {
	Enabled bool   `toml:"enabled"`
	Addr    string `toml:"addr"`
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
			Level:      "info",
			File:       "apex-agent.log",
			MaxSizeMB:  50,
			MaxBackups: 3,
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
level = "info"              # debug | info | warn | error
file = "apex-agent.log"     # 데이터 디렉토리 기준 상대 경로
max_size_mb = 50
max_backups = 3
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
