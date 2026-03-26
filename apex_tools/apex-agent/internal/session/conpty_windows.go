// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build windows

package session

import (
	"fmt"
	"os"
	"sync"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	kernel32                      = windows.NewLazySystemDLL("kernel32.dll")
	procCreatePseudoConsole       = kernel32.NewProc("CreatePseudoConsole")
	procResizePseudoConsole       = kernel32.NewProc("ResizePseudoConsole")
	procClosePseudoConsole        = kernel32.NewProc("ClosePseudoConsole")
	procInitializeProcThreadAttrs = kernel32.NewProc("InitializeProcThreadAttributeList")
	procUpdateProcThreadAttr      = kernel32.NewProc("UpdateProcThreadAttribute")
	procDeleteProcThreadAttrs     = kernel32.NewProc("DeleteProcThreadAttributeList")
)

const _PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016

// conPTYCoord matches the COORD struct layout expected by CreatePseudoConsole.
// The Windows API packs X (columns) in the low 16 bits and Y (rows) in the high 16 bits
// of a single DWORD parameter.
type conPTYCoord struct {
	X, Y int16
}

func coordToDWORD(c conPTYCoord) uintptr {
	return uintptr(*(*uint32)(unsafe.Pointer(&c)))
}

// ConPTY wraps a Windows pseudo-console attached to a child process.
type ConPTY struct {
	hPC       windows.Handle
	pipeIn    *os.File // write end → ConPTY stdin
	pipeOut   *os.File // read end ← ConPTY stdout
	process   *os.Process
	closeOnce sync.Once
}

// NewConPTY creates a ConPTY with the given dimensions and spawns cmdLine inside it.
func NewConPTY(cmdLine string, cols, rows int) (*ConPTY, error) {
	if cols <= 0 || cols > 32767 || rows <= 0 || rows > 32767 {
		return nil, fmt.Errorf("invalid terminal dimensions: cols=%d, rows=%d (must be 1..32767)", cols, rows)
	}

	// Create pipes for ConPTY I/O.
	ptyInR, ptyInW, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("create input pipe: %w", err)
	}
	ptyOutR, ptyOutW, err := os.Pipe()
	if err != nil {
		ptyInR.Close()
		ptyInW.Close()
		return nil, fmt.Errorf("create output pipe: %w", err)
	}

	// CreatePseudoConsole
	size := conPTYCoord{X: int16(cols), Y: int16(rows)}
	var hPC windows.Handle
	r, _, _ := procCreatePseudoConsole.Call(
		coordToDWORD(size),
		ptyInR.Fd(),
		ptyOutW.Fd(),
		0,
		uintptr(unsafe.Pointer(&hPC)),
	)
	if r != 0 {
		ptyInR.Close()
		ptyInW.Close()
		ptyOutR.Close()
		ptyOutW.Close()
		return nil, fmt.Errorf("CreatePseudoConsole: HRESULT 0x%08x", r)
	}

	// Close the pipe ends that the ConPTY now owns.
	ptyInR.Close()
	ptyOutW.Close()

	// Spawn process with ConPTY attached.
	proc, err := spawnInConPTY(cmdLine, hPC)
	if err != nil {
		procClosePseudoConsole.Call(uintptr(hPC))
		ptyInW.Close()
		ptyOutR.Close()
		return nil, fmt.Errorf("spawn process: %w", err)
	}

	return &ConPTY{
		hPC:     hPC,
		pipeIn:  ptyInW,
		pipeOut: ptyOutR,
		process: proc,
	}, nil
}

func spawnInConPTY(cmdLine string, hPC windows.Handle) (*os.Process, error) {
	// Initialize thread attribute list.
	var size uintptr
	procInitializeProcThreadAttrs.Call(0, 1, 0, uintptr(unsafe.Pointer(&size)))
	attrList := make([]byte, size)
	r, _, _ := procInitializeProcThreadAttrs.Call(
		uintptr(unsafe.Pointer(&attrList[0])),
		1, 0, uintptr(unsafe.Pointer(&size)),
	)
	if r == 0 {
		return nil, fmt.Errorf("InitializeProcThreadAttributeList failed")
	}
	defer procDeleteProcThreadAttrs.Call(uintptr(unsafe.Pointer(&attrList[0])))

	// Set pseudo console attribute.
	r, _, _ = procUpdateProcThreadAttr.Call(
		uintptr(unsafe.Pointer(&attrList[0])),
		0,
		_PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
		uintptr(hPC),
		unsafe.Sizeof(hPC),
		0, 0,
	)
	if r == 0 {
		return nil, fmt.Errorf("UpdateProcThreadAttribute failed")
	}

	// CreateProcess with EXTENDED_STARTUPINFO_PRESENT.
	cmdLinePtr, _ := windows.UTF16PtrFromString(cmdLine)
	si := windows.StartupInfoEx{
		StartupInfo:             windows.StartupInfo{Cb: uint32(unsafe.Sizeof(windows.StartupInfoEx{}))},
		ProcThreadAttributeList: (*windows.ProcThreadAttributeList)(unsafe.Pointer(&attrList[0])),
	}
	var pi windows.ProcessInformation
	err := windows.CreateProcess(
		nil, cmdLinePtr, nil, nil, false,
		windows.EXTENDED_STARTUPINFO_PRESENT, nil, nil,
		&si.StartupInfo, &pi,
	)
	if err != nil {
		return nil, fmt.Errorf("CreateProcess: %w", err)
	}
	windows.CloseHandle(pi.Thread)

	proc, _ := os.FindProcess(int(pi.ProcessId))
	return proc, nil
}

func (c *ConPTY) Read(p []byte) (int, error) {
	return c.pipeOut.Read(p)
}

func (c *ConPTY) Write(p []byte) (int, error) {
	return c.pipeIn.Write(p)
}

func (c *ConPTY) Close() error {
	var firstErr error
	c.closeOnce.Do(func() {
		procClosePseudoConsole.Call(uintptr(c.hPC))
		if err := c.pipeIn.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
		if err := c.pipeOut.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
		if c.process != nil {
			c.process.Kill()
			c.process.Wait()
		}
	})
	return firstErr
}

func (c *ConPTY) Resize(cols, rows int) error {
	if cols <= 0 || cols > 32767 || rows <= 0 || rows > 32767 {
		return fmt.Errorf("invalid terminal dimensions: cols=%d, rows=%d (must be 1..32767)", cols, rows)
	}
	size := conPTYCoord{X: int16(cols), Y: int16(rows)}
	r, _, _ := procResizePseudoConsole.Call(
		uintptr(c.hPC),
		coordToDWORD(size),
	)
	if r != 0 {
		return fmt.Errorf("ResizePseudoConsole: HRESULT 0x%08x", r)
	}
	return nil
}

func (c *ConPTY) Pid() int {
	if c.process != nil {
		return c.process.Pid
	}
	return 0
}
