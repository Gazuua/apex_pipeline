// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/cleanup"
)

func cleanupCmd() *cobra.Command {
	var execute bool

	cmd := &cobra.Command{
		Use:   "cleanup",
		Short: "머지 완료 브랜치 정리 (워크트리 + 로컬 + 리모트 + 복사본)",
		Long: `머지 완료된 브랜치를 4단계로 정리합니다.

  1단계: 워크트리 (git worktree remove)
  2단계: 로컬 브랜치 (git branch -D)
  3단계: 리모트 브랜치 (git push origin --delete)
  4단계: 워크스페이스 복사본 로컬 브랜치 (같은 origin의 형제 디렉토리)

기본값은 dry-run입니다. 실제 삭제를 수행하려면 --execute 플래그를 사용하세요.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			cwd, err := os.Getwd()
			if err != nil {
				return fmt.Errorf("getwd: %w", err)
			}

			if execute {
				fmt.Println("==========================================")
				fmt.Println("  apex-agent cleanup — 실행 모드")
				fmt.Println("==========================================")
			} else {
				fmt.Println("==========================================")
				fmt.Println("  apex-agent cleanup — dry-run 모드")
				fmt.Println("==========================================")
			}
			fmt.Println()

			// 핸드오프 활성 브랜치 조회
			activeBranches := map[string]bool{}
			activeResult, listErr := sendHandoffRequest("list-active", nil)
			if listErr == nil {
				if branches, ok := activeResult["branches"].([]any); ok {
					for _, raw := range branches {
						data, _ := json.Marshal(raw)
						var info struct {
							GitBranch string `json:"git_branch"`
						}
						if json.Unmarshal(data, &info) == nil && info.GitBranch != "" {
							activeBranches[info.GitBranch] = true
						}
					}
				}
			}
			if len(activeBranches) > 0 {
				fmt.Printf("활성 핸드오프 브랜치: %d개 (보호됨)\n", len(activeBranches))
			}

			fmt.Println("리모트 정보 갱신 중...")
			fmt.Println()

			cleanupResult, err := cleanup.Run(cwd, execute, activeBranches)
			if err != nil {
				return err
			}

			printCleanupResults(cleanupResult, execute)
			return nil
		},
	}

	cmd.Flags().BoolVar(&execute, "execute", false, "실제 삭제 수행 (기본: dry-run)")
	return cmd
}

func printCleanupResults(result *cleanup.Result, execute bool) {
	// ── Worktrees ──
	fmt.Println("[Worktrees] ================================================")
	if len(result.Worktrees) == 0 && countWorktreeWarnings(result) == 0 {
		fmt.Println("  (정리할 워크트리 없음)")
	} else {
		for _, a := range result.Worktrees {
			if execute {
				fmt.Printf("  삭제: %s [%s]\n", a.Target, a.Branch)
			} else {
				fmt.Printf("  대상: %s [%s] (디렉토리도 삭제됩니다)\n", a.Target, a.Branch)
			}
		}
	}
	printWarnings(result.Warnings, "워크트리")
	fmt.Println()

	// ── Local Branches ──
	fmt.Println("[Local Branches] ===========================================")
	if len(result.Local) == 0 && countLocalWarnings(result) == 0 {
		fmt.Println("  (정리할 로컬 브랜치 없음)")
	} else {
		for _, a := range result.Local {
			if execute {
				fmt.Printf("  삭제: %s\n", a.Branch)
			} else {
				fmt.Printf("  대상: %s\n", a.Branch)
			}
		}
	}
	printWarnings(result.Warnings, "로컬")
	fmt.Println()

	// ── Empty Worktree Dirs ──
	if len(result.EmptyDirs) > 0 {
		fmt.Println("[Empty Worktree Dirs] ======================================")
		for _, a := range result.EmptyDirs {
			if execute {
				fmt.Printf("  빈 디렉토리 삭제: %s\n", a.Target)
			} else {
				fmt.Printf("  대상 (빈 디렉토리): %s\n", a.Target)
			}
		}
		fmt.Println()
	}

	// ── Remote Branches ──
	fmt.Println("[Remote Branches] ==========================================")
	if len(result.Remote) == 0 && countRemoteWarnings(result) == 0 {
		fmt.Println("  (정리할 리모트 브랜치 없음)")
	} else {
		for _, a := range result.Remote {
			if execute {
				fmt.Printf("  삭제: %s\n", a.Target)
			} else {
				fmt.Printf("  대상: %s (머지 완료)\n", a.Target)
			}
		}
	}
	printWarnings(result.Warnings, "리모트")
	fmt.Println()

	// ── Workspace Copies ──
	if len(result.Copies) > 0 {
		fmt.Println("[Workspace Copies] =========================================")
		for _, a := range result.Copies {
			if execute {
				fmt.Printf("  삭제: %s\n", a.Target)
			} else {
				fmt.Printf("  대상: %s (머지 완료)\n", a.Target)
			}
		}
		fmt.Println()
	}

	// ── Summary ──
	fmt.Println("==========================================")
	total := len(result.Worktrees) + len(result.Local) + len(result.EmptyDirs) + len(result.Remote) + len(result.Copies)
	fmt.Printf("  워크트리: %d개\n", len(result.Worktrees))
	fmt.Printf("  로컬 브랜치: %d개\n", len(result.Local))
	fmt.Printf("  빈 워크트리 디렉토리: %d개\n", len(result.EmptyDirs))
	fmt.Printf("  리모트 브랜치: %d개 (머지 완료)\n", len(result.Remote))
	fmt.Printf("  복사본 브랜치: %d개\n", len(result.Copies))
	fmt.Printf("  합계: %d개\n", total)

	if len(result.Warnings) > 0 {
		fmt.Println("  ──────────────────────────────")
		fmt.Printf("  제외 (미머지/변경사항): %d개\n", len(result.Warnings))
	}
	fmt.Println("==========================================")

	if !execute {
		fmt.Println()
		fmt.Println("위 항목을 실제로 삭제하려면:")
		fmt.Println("  apex-agent cleanup --execute")
	}
}

// printWarnings prints warnings that contain the given keyword.
// Warnings are stored as a flat slice; we only print those relevant to the current section.
// Since the Result.Warnings slice contains all warnings mixed together, we print
// all of them only in the first section that has warnings and skip in the rest.
// To keep it simple we print all warnings in the summary only, not per-section.
// This helper is intentionally a no-op here — warnings are printed in the summary.
func printWarnings(_ []string, _ string) {}

// countWorktreeWarnings, countLocalWarnings, countRemoteWarnings are stubs
// used to decide whether to print "nothing to do" for a section.
// Because warnings are mixed, we track presence via the Warnings slice length.
func countWorktreeWarnings(result *cleanup.Result) int {
	count := 0
	for _, w := range result.Warnings {
		if strings.Contains(w, "워크트리") {
			count++
		}
	}
	return count
}

func countLocalWarnings(result *cleanup.Result) int {
	count := 0
	for _, w := range result.Warnings {
		if strings.Contains(w, "로컬") || strings.Contains(w, "변경사항") {
			count++
		}
	}
	return count
}

func countRemoteWarnings(result *cleanup.Result) int {
	count := 0
	for _, w := range result.Warnings {
		if strings.Contains(w, "리모트") {
			count++
		}
	}
	return count
}
