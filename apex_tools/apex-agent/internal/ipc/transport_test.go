// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"runtime"
	"testing"
)

func testAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-test`
	}
	return "/tmp/apex-agent-test.sock"
}

func TestTransport_ListenAndDial(t *testing.T) {
	addr := testAddr()

	ln, err := Listen(addr)
	if err != nil {
		t.Fatalf("Listen: %v", err)
	}
	defer ln.Close()

	done := make(chan error, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			done <- err
			return
		}
		conn.Write([]byte("hello"))
		conn.Close()
		done <- nil
	}()

	conn, err := Dial(addr)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}

	buf := make([]byte, 5)
	n, _ := conn.Read(buf)
	conn.Close()

	if string(buf[:n]) != "hello" {
		t.Errorf("got %q, want 'hello'", buf[:n])
	}

	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
