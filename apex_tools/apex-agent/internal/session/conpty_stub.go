// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build !windows

package session

import "fmt"

// ConPTY is not supported on non-Windows platforms.
type ConPTY struct{}

// NewConPTY returns an error on non-Windows platforms.
func NewConPTY(cmdLine string, cols, rows int) (*ConPTY, error) {
	return nil, fmt.Errorf("ConPTY is only available on Windows")
}

func (c *ConPTY) Read(p []byte) (int, error)  { return 0, fmt.Errorf("not supported") }
func (c *ConPTY) Write(p []byte) (int, error) { return 0, fmt.Errorf("not supported") }
func (c *ConPTY) Close() error                { return nil }
func (c *ConPTY) Resize(cols, rows int) error  { return fmt.Errorf("not supported") }
func (c *ConPTY) Pid() int                    { return 0 }
