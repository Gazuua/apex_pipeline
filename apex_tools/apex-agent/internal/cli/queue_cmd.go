// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
)

func queueCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "queue",
		Short: "빌드/머지 잠금 큐 관리",
	}
	cmd.AddCommand(queueAcquireCmd())
	cmd.AddCommand(queueReleaseCmd())
	cmd.AddCommand(queueStatusCmd())
	cmd.AddCommand(queueBuildCmd())
	cmd.AddCommand(queueBenchmarkCmd())
	return cmd
}

// sendQueueRequest sends a request to the queue module and returns the raw response.
func sendQueueRequest(action string, params any) (*ipc.Response, error) {
	return sendRequest("queue", action, params, getBranchID())
}

// sendQueueRequestMap sends a request and parses the response data as map[string]any.
func sendQueueRequestMap(action string, params any) (map[string]any, error) {
	return sendRequestMap("queue", action, params, getBranchID())
}

// sendQueueAcquire sends a blocking acquire request with extended timeout (35min).
// queue.acquire can block up to 30min waiting for lock, so 10s default timeout is insufficient.
func sendQueueAcquire(params any) (map[string]any, error) {
	return sendRequestMapWithTimeout("queue", "acquire", params, getBranchID(), 35*time.Minute)
}

// projectRoot returns the project root directory.
// It walks up from the current working directory until it finds a directory
// containing build.bat (the C++ build script).
func projectRoot() (string, error) {
	cwd, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("getwd: %w", err)
	}
	dir := cwd
	for {
		if _, err := os.Stat(filepath.Join(dir, "build.bat")); err == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			// Reached filesystem root without finding build.bat.
			return "", fmt.Errorf("project root not found: build.bat not found above %s", cwd)
		}
		dir = parent
	}
}

// ── queue acquire ──

func queueAcquireCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "acquire <channel>",
		Short: "채널 잠금 획득 (블로킹)",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			channel := args[0]
			branch := getBranchID()
			params := map[string]any{
				"channel": channel,
				"branch":  branch,
				"pid":     os.Getpid(),
			}
			_, err := sendQueueAcquire(params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			fmt.Printf("[queue-lock] lock acquired: %s (branch=%s)\n", channel, branch)
			return nil
		},
	}
}

// ── queue release ──

func queueReleaseCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "release <channel>",
		Short: "채널 잠금 해제",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			channel := args[0]
			params := map[string]any{
				"channel": channel,
			}
			_, err := sendQueueRequestMap("release", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			fmt.Printf("[queue-lock] lock released: %s\n", channel)
			return nil
		},
	}
}

// printQueueStatus formats and prints the status of a queue channel.
func printQueueStatus(channel string, result map[string]any) {
	active := result["active"]
	waitingRaw, _ := result["waiting"].([]any)
	depth := len(waitingRaw)

	if active == nil || active == false {
		fmt.Printf("[queue-lock] channel=%s LOCK=FREE queue_depth=%d\n", channel, depth)
	} else {
		activeBranch := ""
		if activeMap, ok := active.(map[string]any); ok {
			activeBranch, _ = activeMap["branch"].(string)
		}
		if activeBranch != "" {
			fmt.Printf("[queue-lock] channel=%s LOCK=HELD by %s queue_depth=%d\n", channel, activeBranch, depth)
		} else {
			fmt.Printf("[queue-lock] channel=%s LOCK=HELD queue_depth=%d\n", channel, depth)
		}
	}
}

// ── queue status ──

func queueStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status <channel>",
		Short: "채널 잠금 상태 조회",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			channel := args[0]
			result, err := sendQueueRequestMap("status", map[string]any{"channel": channel})
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			printQueueStatus(channel, result)
			return nil
		},
	}
}

// runWithBuildLock acquires the build channel lock, executes the command returned
// by makeCmd, then releases the lock. Handles lock release both on success and
// failure (via defer). label is used in log messages (e.g. "build", "benchmark").
func runWithBuildLock(label string, makeCmd func() (*exec.Cmd, string)) error {
	branch := getBranchID()

	// Acquire build lock.
	acquireParams := map[string]any{
		"channel": "build",
		"branch":  branch,
		"pid":     os.Getpid(),
	}
	_, err := sendQueueAcquire(acquireParams)
	if err != nil {
		return fmt.Errorf("[queue-lock] failed to acquire build lock: %w", err)
	}
	fmt.Printf("[queue-lock] lock acquired: build (branch=%s)\n", branch)

	// Ensure lock is released on exit.
	released := false
	defer func() {
		if !released {
			releaseParams := map[string]any{"channel": "build"}
			_, _ = sendQueueRequestMap("release", releaseParams)
			fmt.Printf("[queue-lock] lock released: build\n")
		}
	}()

	// Build the command and get its display string.
	execCmd, displayStr := makeCmd()
	execCmd.Stdout = os.Stdout
	execCmd.Stderr = os.Stderr
	execCmd.Stdin = os.Stdin

	fmt.Printf("[queue-lock] starting %s: %s\n", label, displayStr)

	// Use Start+Wait instead of Run to transfer lock PID to child process.
	// This ensures CleanupStale checks the build process, not the wrapper.
	if err := execCmd.Start(); err != nil {
		return fmt.Errorf("[queue-lock] failed to start %s: %w", label, err)
	}

	// Transfer lock ownership: parent PID → child (build) PID.
	// If parent is killed, CleanupStale will check child PID (still alive).
	childPID := execCmd.Process.Pid
	updateParams := map[string]any{"channel": "build", "branch": branch, "pid": childPID}
	if _, err := sendQueueRequestMap("update-pid", updateParams); err != nil {
		fmt.Fprintf(os.Stderr, "[queue-lock] warning: failed to update lock PID to child %d: %v\n", childPID, err)
	} else {
		fmt.Printf("[queue-lock] lock PID transferred to child process %d\n", childPID)
	}

	runErr := execCmd.Wait()

	// Release lock explicitly before printing result.
	releaseParams := map[string]any{"channel": "build"}
	_, _ = sendQueueRequestMap("release", releaseParams)
	released = true
	fmt.Printf("[queue-lock] lock released: build\n")

	if runErr != nil {
		exitCode := 1
		if exitErr, ok := runErr.(*exec.ExitError); ok {
			exitCode = exitErr.ExitCode()
		}
		fmt.Printf("[queue-lock] %s FAILED (exit code: %d)\n", label, exitCode)
		os.Exit(exitCode)
	}

	fmt.Printf("[queue-lock] %s completed successfully\n", label)
	return nil
}

// ── queue build ──

func queueBuildCmd() *cobra.Command {
	return &cobra.Command{
		Use:                "build <preset> [extra args...]",
		Short:              "빌드 잠금 획득 후 build.bat 실행",
		Args:               cobra.MinimumNArgs(1),
		DisableFlagParsing: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			preset := args[0]
			extraArgs := args[1:]

			fmt.Printf("[queue-lock] build requested: preset=%s, args=%s, branch=%s\n",
				preset, formatArgs(extraArgs), getBranchID())

			return runWithBuildLock("build", func() (*exec.Cmd, string) {
				root, err := projectRoot()
				if err != nil {
					// projectRoot failure: create a command that will fail immediately.
					// The error is printed via stderr.
					fmt.Fprintf(os.Stderr, "[queue-lock] %v\n", err)
					return exec.Command("false"), "false"
				}

				buildBat := filepath.Join(root, "build.bat")
				display := strings.Join(append([]string{buildBat, preset}, extraArgs...), " ")

				var c *exec.Cmd
				if runtime.GOOS == "windows" {
					cmdArgs := append([]string{"/c", buildBat, preset}, extraArgs...)
					c = exec.Command("cmd.exe", cmdArgs...)
				} else {
					shArgs := append([]string{buildBat, preset}, extraArgs...)
					c = exec.Command("bash", shArgs...)
				}
				return c, display
			})
		},
	}
}

// ── queue benchmark ──

func queueBenchmarkCmd() *cobra.Command {
	return &cobra.Command{
		Use:                "benchmark <exe> [benchmark args...]",
		Short:              "빌드 잠금 획득 후 벤치마크 실행",
		Long:               "build 채널 lock을 공유하여 빌드/벤치마크 상호배제를 보장한다.\n직접 bench_* 실행은 validate-build hook이 차단하므로 이 커맨드를 사용해야 한다.",
		Args:               cobra.MinimumNArgs(1),
		DisableFlagParsing: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			exe := args[0]
			benchArgs := args[1:]

			// 명령 인젝션 방어: bench_ prefix 검증
			base := filepath.Base(exe)
			if !strings.HasPrefix(base, "bench_") {
				return fmt.Errorf("benchmark executable must start with 'bench_' prefix, got %q", base)
			}
			// 프로젝트 루트 하위인지 확인 (절대경로·상대경로 모두 검증)
			root, rootErr := projectRoot()
			if rootErr != nil {
				return fmt.Errorf("cannot verify benchmark path: %w", rootErr)
			}
			exeAbs := exe
			if !filepath.IsAbs(exe) {
				absPath, absErr := filepath.Abs(exe)
				if absErr != nil {
					return fmt.Errorf("cannot resolve relative benchmark path %q: %w", exe, absErr)
				}
				exeAbs = absPath
			}
			rel, relErr := filepath.Rel(root, exeAbs)
			if relErr != nil || strings.HasPrefix(rel, "..") {
				return fmt.Errorf("benchmark executable must be under project root %q, got %q", root, exeAbs)
			}

			fmt.Printf("[queue-lock] benchmark requested: exe=%s, args=%s, branch=%s\n",
				exe, formatArgs(benchArgs), getBranchID())

			return runWithBuildLock("benchmark", func() (*exec.Cmd, string) {
				display := strings.Join(append([]string{exe}, benchArgs...), " ")
				return exec.Command(exe, benchArgs...), display
			})
		},
	}
}

// formatArgs formats a slice of args for display. Returns "none" if empty.
func formatArgs(args []string) string {
	if len(args) == 0 {
		return "none"
	}
	return strings.Join(args, " ")
}

