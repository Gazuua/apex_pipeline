// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import "testing"

func TestParseID(t *testing.T) {
	tests := []struct {
		name    string
		arg     string
		want    int
		wantErr bool
	}{
		{name: "positive integer", arg: "42", want: 42, wantErr: false},
		{name: "one", arg: "1", want: 1, wantErr: false},
		{name: "large number", arg: "99999", want: 99999, wantErr: false},
		{name: "zero", arg: "0", want: 0, wantErr: false},
		{name: "negative", arg: "-1", want: -1, wantErr: false},
		{name: "non-integer string", arg: "abc", want: 0, wantErr: true},
		{name: "empty string", arg: "", want: 0, wantErr: true},
		{name: "float", arg: "3.14", want: 3, wantErr: false}, // Sscanf %d parses leading int
		{name: "leading spaces", arg: " 5", want: 5, wantErr: false}, // Sscanf %d skips leading whitespace
		{name: "mixed", arg: "12abc", want: 12, wantErr: false}, // Sscanf %d parses leading int
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := parseID(tt.arg)
			if (err != nil) != tt.wantErr {
				t.Errorf("parseID(%q) error = %v, wantErr %v", tt.arg, err, tt.wantErr)
				return
			}
			if !tt.wantErr && got != tt.want {
				t.Errorf("parseID(%q) = %d, want %d", tt.arg, got, tt.want)
			}
		})
	}
}
