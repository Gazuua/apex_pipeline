// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import "testing"

func TestIsDaemonManagementCommand(t *testing.T) {
	tests := []struct {
		command string
		want    bool
	}{
		// 통과해야 하는 데몬 관리 명령
		{"./apex_tools/apex-agent/run-hook daemon start", true},
		{"apex-agent daemon stop", true},
		{"apex-agent daemon status", true},
		{"apex-agent daemon run", true},
		{"./apex_tools/apex-agent/run-hook plugin setup", true},
		{`"$LOCALAPPDATA/apex-agent/apex-agent.exe" daemon start`, true},

		// 차단해야 하는 일반 명령
		{"git commit -m 'test'", false},
		{"go build ./...", false},
		{"echo hello", false},
		{"", false},

		// git commit 메시지에 daemon 키워드가 포함된 경우 — 바이패스 금지
		{`git commit -m "fix daemon startup issue"`, false},
		{`git commit -m "daemon start bug"`, false},

		// 체인 명령에서 daemon 관리가 포함된 경우
		{"apex-agent daemon stop && cp binary $LOCALAPPDATA/apex-agent/", true},
	}

	for _, tt := range tests {
		t.Run(tt.command, func(t *testing.T) {
			if got := isDaemonManagementCommand(tt.command); got != tt.want {
				t.Errorf("isDaemonManagementCommand(%q) = %v, want %v", tt.command, got, tt.want)
			}
		})
	}
}
