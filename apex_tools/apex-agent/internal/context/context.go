// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package context

import (
	"context"
	"encoding/json"
	"fmt"
	"os/exec"
	"strings"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

var ml = log.WithModule("context")

// Generate produces the session context string for Claude.
// workspaceRoot is the absolute path to the project root.
func Generate(workspaceRoot string) string {
	ml.Debug("generating session context", "workspace", workspaceRoot)
	var b strings.Builder

	b.WriteString("=== Project Context (auto-injected) ===\n")

	// Git status
	b.WriteString("\n--- Git Status ---\n")
	branch := gitCurrentBranch(workspaceRoot)
	fmt.Fprintf(&b, "Branch: %s\n", branch)

	// Auto-fetch main if on main branch
	if branch == "main" {
		b.WriteString("\nAuto-fetching latest main...\n")
		if out, _ := gitExec(workspaceRoot, "fetch", "origin", "main"); out != "" {
			for _, line := range strings.Split(out, "\n") {
				fmt.Fprintf(&b, "  %s\n", line)
			}
		}
		if out, _ := gitExec(workspaceRoot, "pull", "origin", "main"); out != "" {
			for _, line := range strings.Split(out, "\n") {
				fmt.Fprintf(&b, "  %s\n", line)
			}
		}
	}

	// Status + recent commits
	b.WriteString("\n")
	if out, _ := gitExec(workspaceRoot, "status", "--short"); out != "" {
		b.WriteString(out)
		b.WriteString("\n")
	}
	b.WriteString("Recent commits:\n")
	if out, _ := gitExec(workspaceRoot, "log", "--oneline", "-5"); out != "" {
		b.WriteString(out)
		b.WriteString("\n")
	}

	// Branch handoff status via daemon IPC (non-main branches)
	if branch != "" && branch != "main" {
		branchID := workspaceID(workspaceRoot)
		appendHandoffStatus(&b, branchID)
	}

	b.WriteString("=== End Project Context ===\n")
	return b.String()
}

// workspaceID extracts the workspace branch identifier from the project root.
func workspaceID(workspaceRoot string) string {
	return platform.WorkspaceID(workspaceRoot)
}

// branchInfo mirrors the daemon's Branch struct fields we care about.
type branchInfo struct {
	Branch     string `json:"Branch"`
	Workspace  string `json:"Workspace"`
	Status     string `json:"Status"`
	BacklogIDs []int  `json:"BacklogIDs"`
	Summary    string `json:"Summary"`
	UpdatedAt  string `json:"UpdatedAt"`
}

// appendHandoffStatus queries the daemon for handoff status and writes it to b.
// On daemon connection failure, it writes a warning and skips the section (graceful degradation).
func appendHandoffStatus(b *strings.Builder, branchID string) {
	b.WriteString("\n--- Branch Handoff ---\n")

	client := ipc.NewClient(platform.SocketPath())
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	resp, err := client.Send(ctx, "handoff", "get-branch", map[string]string{
		"branch": branchID,
	}, "")
	if err != nil {
		ml.Warn("daemon unavailable for handoff status", "error", err)
		b.WriteString("WARNING: daemon 연결 실패 — 핸드오프 상태를 조회할 수 없습니다.\n")
		b.WriteString("  → daemon 시작: apex-agent daemon start\n")
		return
	}
	if resp.Error != "" {
		ml.Warn("handoff get-branch error", "error", resp.Error)
		fmt.Fprintf(b, "WARNING: 핸드오프 조회 오류: %s\n", resp.Error)
		return
	}

	// Parse response data
	var result map[string]json.RawMessage
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		ml.Warn("failed to parse handoff response", "error", err)
		return
	}

	rawBranch, ok := result["branch"]
	if !ok || string(rawBranch) == "null" || len(rawBranch) == 0 {
		// Not registered
		fmt.Fprintf(b, "WARNING [BLOCKED]: 이 워크스페이스(%s)가 핸드오프 시스템에 미등록 상태!\n", branchID)
		b.WriteString("  모든 Edit/Write/git commit이 차단됩니다.\n")
		b.WriteString("  → 즉시 실행: apex-agent handoff notify start --scopes <s> --summary \"설명\" [--backlog <N>]\n")
		b.WriteString("  (설계 불필요 시: apex-agent handoff notify start --skip-design --scopes <s> --summary \"설명\")\n")
		return
	}

	var info branchInfo
	if err := json.Unmarshal(rawBranch, &info); err != nil {
		ml.Warn("failed to parse branch info", "error", err)
		return
	}

	fmt.Fprintf(b, "Registered: %s\n", branchID)

	// Backlog IDs
	if len(info.BacklogIDs) > 0 {
		ids := make([]string, len(info.BacklogIDs))
		for i, id := range info.BacklogIDs {
			ids[i] = fmt.Sprintf("BACKLOG-%d", id)
		}
		fmt.Fprintf(b, "  backlogs: %s\n", strings.Join(ids, ", "))
	}

	// Status + guidance
	fmt.Fprintf(b, "  status: %s\n", info.Status)
	switch info.Status {
	case "STARTED":
		b.WriteString("  → 다음: notify design (설계 완료 시) 또는 notify start --skip-design (설계 불필요 시)\n")
	case "DESIGN_NOTIFIED":
		b.WriteString("  → 다음: notify plan (구현 계획 완료 시)\n")
	case "IMPLEMENTING":
		b.WriteString("  → 구현 진행 중 (소스 편집 허용)\n")
	}
}

// gitCurrentBranch returns the current git branch name.
func gitCurrentBranch(root string) string {
	branch, _ := platform.GitCurrentBranch(root)
	return branch
}

// gitExec runs a git command in the given root and returns combined output.
func gitExec(root string, args ...string) (string, error) {
	cmdArgs := append([]string{"-C", root}, args...)
	out, err := exec.Command("git", cmdArgs...).CombinedOutput()
	return strings.TrimSpace(string(out)), err
}
