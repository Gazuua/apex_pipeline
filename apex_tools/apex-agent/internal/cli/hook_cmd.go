// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
)

func hookCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "hook",
		Short: "Hook 검증 게이트",
	}
	cmd.AddCommand(hookValidateBuildCmd())
	cmd.AddCommand(hookValidateMergeCmd())
	cmd.AddCommand(hookValidateHandoffCmd())
	cmd.AddCommand(hookHandoffProbeCmd())
	cmd.AddCommand(hookEnforceRebaseCmd())
	cmd.AddCommand(hookValidateBacklogCmd())
	return cmd
}

func hookValidateBuildCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-build",
		Short: "빌드 도구 직접 호출 차단",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, _, err := readHookInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] error: hook input parse failed: %v (blocking)\n", err)
				os.Exit(2) // fail-close: 데몬 비의존 hook → 파싱 실패 시 차단
			}
			if err := hook.ValidateBuild(command); err != nil {
				fmt.Fprintln(os.Stderr, err.Error())
				os.Exit(2)
			}
			return nil
		},
	}
}

func hookValidateMergeCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-merge",
		Short: "머지 잠금 미획득 차단",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, _, err := readHookInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] warning: hook input parse failed: %v (allowing)\n", err)
				return nil // parse error → allow
			}
			if !containsGhPrMerge(command) {
				return nil
			}

			// Require --delete-branch to prevent stale remote branches
			if !strings.Contains(command, "--delete-branch") {
				fmt.Fprintln(os.Stderr, "차단: gh pr merge에 --delete-branch 플래그를 추가하세요.")
				os.Exit(2)
			}

			// Check merge lock via daemon IPC (DB-based queue)
			result, err := sendRequestMap("queue", "status", map[string]any{"channel": "merge"}, "")
			if err != nil {
				// Daemon unavailable → fail-open (allow merge)
				return nil
			}
			if result["active"] == nil {
				fmt.Fprintln(os.Stderr, "차단: 먼저 apex-agent queue merge acquire를 실행하세요.")
				os.Exit(2)
			}
			// Verify lock holder matches current workspace
			if activeEntry, ok := result["active"].(map[string]any); ok {
				if holder, _ := activeEntry["branch"].(string); holder != "" {
					myBranch := getBranchID()
					if holder != myBranch {
						fmt.Fprintf(os.Stderr, "차단: merge lock 소유자가 %s입니다 (현재: %s)\n", holder, myBranch)
						os.Exit(2)
					}
				}
			}
			return nil
		},
	}
}

func hookEnforceRebaseCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "enforce-rebase",
		Short: "push/PR 전 자동 리베이스",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, _, err := readHookInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] warning: hook input parse failed: %v (allowing)\n", err)
				return nil
			}
			// Determine project root from cwd
			cwd, _ := os.Getwd()
			msg, err := hook.EnforceRebase(command, cwd)
			if err != nil {
				fmt.Fprintln(os.Stderr, err.Error())
				os.Exit(2)
			}
			if msg != "" {
				fmt.Fprintln(os.Stderr, msg)
			}
			return nil
		},
	}
}

// readHookInput reads Claude Code hook JSON from stdin.
// Uses json.Decoder to avoid blocking on unclosed stdin pipe.
// Returns (command, cwd, error).
func readHookInput() (string, string, error) {
	var input struct {
		ToolInput struct {
			Command string `json:"command"`
		} `json:"tool_input"`
		Cwd string `json:"cwd"`
	}
	if err := json.NewDecoder(os.Stdin).Decode(&input); err != nil {
		return "", "", err
	}
	return input.ToolInput.Command, input.Cwd, nil
}

func hookValidateBacklogCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-backlog",
		Short: "BACKLOG 파일 직접 편집 차단",
		RunE: func(cmd *cobra.Command, args []string) error {
			filePath, _, err := readHandoffProbeInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] error: hook input parse failed: %v (blocking)\n", err)
				os.Exit(2) // fail-close: 데몬 비의존 hook → 파싱 실패 시 차단
			}
			if err := hook.ValidateBacklog(filePath); err != nil {
				fmt.Fprintln(os.Stderr, err.Error())
				os.Exit(2)
			}
			return nil
		},
	}
}

// containsGhPrMerge checks if "gh pr merge" appears as an actual command
// in a shell pipeline, not inside a quoted string (e.g. git commit messages).
// Splits on shell operators (&&, ||, ;) and checks each segment.
func containsGhPrMerge(command string) bool {
	// Split on shell command separators
	for _, sep := range []string{"&&", "||", ";"} {
		parts := strings.Split(command, sep)
		if len(parts) > 1 {
			for _, part := range parts {
				if containsGhPrMerge(strings.TrimSpace(part)) {
					return true
				}
			}
			return false
		}
	}
	// Single command: check if it starts with "gh pr merge" (after trimming)
	trimmed := strings.TrimSpace(command)
	return strings.HasPrefix(trimmed, "gh pr merge")
}
