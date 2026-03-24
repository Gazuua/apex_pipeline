// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

// hookValidateHandoffCmd handles the Bash PreToolUse hook for handoff validation:
//  1. Notification probe (every call)
//  2. Commit gate (git commit)
//  3. Merge gate (gh pr merge)
func hookValidateHandoffCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-handoff",
		Short: "핸드오프 Bash hook 게이트 (커밋/머지/알림 프로브)",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, cwd, err := readHookInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] warning: hook input parse failed: %v (allowing)\n", err)
				return nil // malformed input → allow
			}
			if command == "" {
				return nil
			}

			// main/master 브랜치는 스킵
			gitBranch, err := gitCurrentBranch(cwd)
			if err != nil || gitBranch == "main" || gitBranch == "master" {
				return nil
			}

			// feature/bugfix 브랜치만 체크
			if !isFeatureBranch(gitBranch) {
				return nil
			}

			// workspace ID + git branch로 daemon에서 등록된 브랜치 조회
			branch, _ := resolveHandoffBranch(cwd, gitBranch)
			if branch == "" {
				// 미등록 브랜치: git commit은 차단, 그 외는 통과
				if isGitCommit(command) {
					fmt.Fprintln(os.Stderr, "차단: 핸드오프 미등록 브랜치에서 커밋 불가. 'apex-agent handoff notify start'로 등록하세요.")
					os.Exit(2)
				}
				return nil
			}

			// 1) commit 게이트
			if isGitCommit(command) {
				resp, err := sendHandoffRaw("validate-commit", map[string]any{"branch": branch})
				if err != nil {
					// 데몬 연결 실패 시 통과 (graceful degradation)
					return nil
				}
				if resp.Error != "" {
					fmt.Fprintln(os.Stderr, resp.Error)
					os.Exit(2)
				}
			}

			// 2) gh pr merge 게이트
			if strings.Contains(command, "gh pr merge") {
				resp, err := sendHandoffRaw("validate-merge-gate", map[string]any{"branch": branch})
				if err != nil {
					return nil
				}
				if resp.Error != "" {
					fmt.Fprintln(os.Stderr, resp.Error)
					os.Exit(2)
				}
			}

			return nil
		},
	}
}

// hookHandoffProbeCmd handles the Edit|Write PreToolUse hook:
//  1. Registration check (block all edits if not registered)
//  2. Status-based source gate (started/design-notified → block source files)
func hookHandoffProbeCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "handoff-probe",
		Short: "핸드오프 Edit/Write hook 게이트 (등록/상태/알림)",
		RunE: func(cmd *cobra.Command, args []string) error {
			filePath, cwd, err := readHandoffProbeInput()
			if err != nil {
				fmt.Fprintf(os.Stderr, "[apex-agent] warning: hook input parse failed: %v (allowing)\n", err)
				return nil // malformed input → allow
			}

			// main/master 브랜치 스킵
			gitBranch, err := gitCurrentBranch(cwd)
			if err != nil || gitBranch == "main" || gitBranch == "master" {
				return nil
			}

			// feature/bugfix 브랜치만 체크
			if !isFeatureBranch(gitBranch) {
				return nil
			}

			// workspace ID + git branch로 daemon에서 등록된 브랜치 조회
			branch, _ := resolveHandoffBranch(cwd, gitBranch)
			if branch == "" {
				fmt.Fprintln(os.Stderr, "차단: 핸드오프 미등록")
				os.Exit(2)
			}

			// 등록 확인 + 상태 기반 소스 게이트
			resp, err := sendHandoffRaw("validate-edit", map[string]any{
				"branch":    branch,
				"file_path": filePath,
			})
			if err != nil {
				return nil // 데몬 연결 실패 시 통과
			}
			if resp.Error != "" {
				fmt.Fprintln(os.Stderr, resp.Error)
				os.Exit(2)
			}

			return nil
		},
	}
}

// resolveHandoffBranch resolves the daemon branch key using workspace ID (primary)
// and git branch name (fallback). Returns empty string if not found.
func resolveHandoffBranch(cwd, gitBranch string) (string, error) {
	wsID := getBranchID()
	if cwd != "" {
		wsID = platform.WorkspaceID(cwd)
	}

	resp, err := sendHandoffRaw("resolve-branch", map[string]any{
		"workspace_id": wsID,
		"git_branch":   gitBranch,
	})
	if err != nil {
		return "", err
	}
	if resp.Error != "" {
		return "", fmt.Errorf("%s", resp.Error)
	}

	var result map[string]any
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		return "", err
	}
	if found, _ := result["found"].(bool); found {
		if branch, ok := result["branch"].(string); ok {
			return branch, nil
		}
	}
	return "", nil
}

// --- helpers ---

// readHandoffProbeInput reads Edit/Write hook JSON from stdin.
// Uses json.Decoder to avoid blocking on unclosed stdin pipe.
func readHandoffProbeInput() (string, string, error) {
	var input struct {
		ToolInput struct {
			FilePath string `json:"file_path"`
		} `json:"tool_input"`
		Cwd string `json:"cwd"`
	}
	if err := json.NewDecoder(os.Stdin).Decode(&input); err != nil {
		return "", "", err
	}
	return input.ToolInput.FilePath, input.Cwd, nil
}

// sendHandoffRaw sends a request to the handoff module and returns the raw response.
func sendHandoffRaw(action string, params any) (*ipc.Response, error) {
	return sendRequest("handoff", action, params, "")
}

// sendHandoffRequest sends a request and parses the data as map[string]any.
func sendHandoffRequest(action string, params any) (map[string]any, error) {
	return sendRequestMap("handoff", action, params, "")
}

// gitCurrentBranch returns the current git branch for the given working directory.
func gitCurrentBranch(cwd string) (string, error) {
	return platform.GitCurrentBranch(cwd)
}

// isGitCommit checks if the command is a git commit invocation.
func isGitCommit(command string) bool {
	return strings.Contains(command, "git commit")
}

// isFeatureBranch returns true for feature/* and bugfix/* branches.
func isFeatureBranch(branch string) bool {
	return strings.HasPrefix(branch, "feature/") || strings.HasPrefix(branch, "bugfix/")
}
