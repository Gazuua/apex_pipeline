// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"testing"
)

func TestIsDialError(t *testing.T) {
	tests := []struct {
		err  error
		want bool
	}{
		{fmt.Errorf("dial: connection refused"), true},
		{fmt.Errorf("dial: %w", fmt.Errorf("pipe not found")), true},
		{fmt.Errorf("read: unexpected EOF"), false},
		{fmt.Errorf("timeout"), false},
		{nil, false},
	}

	for _, tt := range tests {
		name := "nil"
		if tt.err != nil {
			name = tt.err.Error()
		}
		t.Run(name, func(t *testing.T) {
			if got := isDialError(tt.err); got != tt.want {
				t.Errorf("isDialError(%v) = %v, want %v", tt.err, got, tt.want)
			}
		})
	}
}
