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
		Short: "PR 명령 검증 (머지 잠금 + delete-branch + base main 강제)",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, _, err := readHookInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] warning: hook input parse failed: %v (allowing)\n", err)
				return nil // parse error → allow
			}

			// gh pr create: enforce --base main (prevent stacked PRs)
			if subcmd := findShellSubcommand(command, "gh pr create"); subcmd != "" {
				if base := extractFlag(subcmd, "--base"); base != "" && base != "main" {
					fmt.Fprintf(os.Stderr, "차단: gh pr create의 --base는 main만 허용됩니다 (현재: %s)\n", base)
					os.Exit(2)
				}
			}

			// gh pr merge: require --delete-branch + lock
			if containsShellCommand(command, "gh pr merge") {
				// Require --delete-branch to prevent stale remote branches
				if !strings.Contains(command, "--delete-branch") {
					fmt.Fprintln(os.Stderr, "차단: gh pr merge에 --delete-branch 플래그를 추가하세요.")
					os.Exit(2)
				}

				// Check merge lock via daemon IPC (DB-based queue)
				result, err := sendRequestMap("queue", "status", map[string]any{"channel": "merge"}, "")
				if err != nil {
					fmt.Fprintf(os.Stderr, "[hook] error: daemon unreachable — run 'apex-agent daemon start'\n")
					os.Exit(2)
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

// containsShellCommand checks if prefix appears as an actual command
// in a shell pipeline, not inside a quoted string (e.g. git commit messages).
// Splits on shell operators (&&, ||, ;) and checks each segment.
func containsShellCommand(command, prefix string) bool {
	return findShellSubcommand(command, prefix) != ""
}

// findShellSubcommand returns the specific subcommand that starts with prefix,
// or "" if not found. Splits on shell operators to isolate each command.
func findShellSubcommand(command, prefix string) string {
	for _, sep := range []string{"&&", "||", ";"} {
		parts := strings.Split(command, sep)
		if len(parts) > 1 {
			for _, part := range parts {
				if found := findShellSubcommand(strings.TrimSpace(part), prefix); found != "" {
					return found
				}
			}
			return ""
		}
	}
	trimmed := strings.TrimSpace(command)
	if strings.HasPrefix(trimmed, prefix) {
		return trimmed
	}
	return ""
}

// extractFlag extracts the value of a CLI flag from a command string.
// Handles both "--flag value" and "--flag=value" formats.
func extractFlag(command, flag string) string {
	idx := strings.Index(command, flag)
	if idx == -1 {
		return ""
	}
	rest := command[idx+len(flag):]
	if strings.HasPrefix(rest, "=") {
		if fields := strings.Fields(rest[1:]); len(fields) > 0 {
			return fields[0]
		}
	} else if rest == "" || rest[0] == ' ' {
		if fields := strings.Fields(rest); len(fields) > 0 {
			return fields[0]
		}
	}
	return ""
}
