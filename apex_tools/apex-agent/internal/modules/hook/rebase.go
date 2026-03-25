// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"regexp"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"
)

// EnforceRebase checks if the current branch needs rebasing on origin/main.
// Only triggers on git push and gh pr create commands.
// Delegates actual rebase to workflow.RebaseOnMain().
func EnforceRebase(command, projectRoot string) (string, error) {
	// Only intercept git push and gh pr create
	if !isGitPush(command) && !isGHPRCreate(command) {
		return "", nil
	}

	// Get current branch
	branch, err := platform.GitCurrentBranch(projectRoot)
	if err != nil {
		return "", nil // can't determine branch → allow
	}

	// Skip main/master/detached
	if branch == "main" || branch == "master" || branch == "HEAD" {
		return "", nil
	}

	// Delegate to workflow package (includes BACKLOG-157 abort error handling)
	return workflow.RebaseOnMain(projectRoot)
}

var gitPushPattern = regexp.MustCompile(`\bgit\b.*\bpush\b`)
var ghPRCreatePattern = regexp.MustCompile(`\bgh\b.*\bpr\b.*\bcreate\b`)

func isGitPush(cmd string) bool {
	return gitPushPattern.MatchString(cmd)
}

func isGHPRCreate(cmd string) bool {
	return ghPRCreatePattern.MatchString(cmd)
}
