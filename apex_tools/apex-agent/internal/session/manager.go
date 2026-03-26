// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("session")

const (
	StatusStop     = "STOP"
	StatusManaged  = "MANAGED"
	StatusExternal = "EXTERNAL"
)

// SessionInfo holds runtime state of an active session.
type SessionInfo struct {
	WorkspaceID string    `json:"workspace_id"`
	SessionID   string    `json:"session_id"`
	Status      string    `json:"status"`
	PID         int       `json:"pid"`
	StartedAt   time.Time `json:"started_at"`
	LogPath     string    `json:"log_path"`

	term    Terminal
	ring    *RingBuffer
	logFile *os.File
	clients map[chan []byte]struct{} // WebSocket subscribers
	mu      sync.Mutex
	done    chan struct{} // closed when read pump exits
}

// ManagerConfig holds session manager settings.
type ManagerConfig struct {
	OutputBufferLines int
	LogDir            string
}

// Manager manages active terminal sessions.
type Manager struct {
	cfg      ManagerConfig
	sessions map[string]*SessionInfo // keyed by workspace_id
	mu       sync.RWMutex
}

// NewManager creates a session Manager.
func NewManager(cfg ManagerConfig) *Manager {
	if cfg.OutputBufferLines <= 0 {
		cfg.OutputBufferLines = 500
	}
	return &Manager{
		cfg:      cfg,
		sessions: make(map[string]*SessionInfo),
	}
}

// Register adds an already-created terminal to the manager.
// Starts the read pump that broadcasts output to WebSocket clients and log file.
func (m *Manager) Register(workspaceID, sessionID string, term Terminal) (*SessionInfo, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if _, exists := m.sessions[workspaceID]; exists {
		return nil, fmt.Errorf("session already active for %s", workspaceID)
	}

	// Ensure log directory exists.
	logDir := filepath.Join(m.cfg.LogDir, workspaceID)
	os.MkdirAll(logDir, 0o755)
	logPath := filepath.Join(logDir, time.Now().Format("20060102_150405")+".log")
	logFile, err := os.Create(logPath)
	if err != nil {
		ml.Warn("session log file creation failed", "path", logPath, "err", err)
	}

	info := &SessionInfo{
		WorkspaceID: workspaceID,
		SessionID:   sessionID,
		Status:      StatusManaged,
		PID:         term.Pid(),
		StartedAt:   time.Now(),
		LogPath:     logPath,
		term:        term,
		ring:        NewRingBuffer(m.cfg.OutputBufferLines),
		logFile:     logFile,
		clients:     make(map[chan []byte]struct{}),
		done:        make(chan struct{}),
	}

	m.sessions[workspaceID] = info
	go info.readPump()

	ml.Info("session registered", "workspace", workspaceID, "session_id", sessionID, "pid", term.Pid())
	return info, nil
}

// Get returns session info for a workspace, or nil if not active.
func (m *Manager) Get(workspaceID string) *SessionInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.sessions[workspaceID]
}

// List returns all active sessions.
func (m *Manager) List() []*SessionInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()
	result := make([]*SessionInfo, 0, len(m.sessions))
	for _, s := range m.sessions {
		result = append(result, s)
	}
	return result
}

// Stop terminates a session and removes it from the manager.
func (m *Manager) Stop(_ context.Context, workspaceID string) error {
	m.mu.Lock()
	info, exists := m.sessions[workspaceID]
	if !exists {
		m.mu.Unlock()
		return fmt.Errorf("no active session for %s", workspaceID)
	}
	delete(m.sessions, workspaceID)
	m.mu.Unlock()

	if err := info.term.Close(); err != nil {
		ml.Warn("terminal close error", "workspace", workspaceID, "err", err)
	}

	// Wait for read pump to finish.
	<-info.done

	if info.logFile != nil {
		info.logFile.Close()
	}

	// Close all subscriber channels.
	info.mu.Lock()
	for ch := range info.clients {
		close(ch)
		delete(info.clients, ch)
	}
	info.mu.Unlock()

	ml.Info("session stopped", "workspace", workspaceID)
	return nil
}

// StopAll terminates all active sessions. Called during graceful shutdown.
func (m *Manager) StopAll(ctx context.Context) {
	m.mu.RLock()
	ids := make([]string, 0, len(m.sessions))
	for id := range m.sessions {
		ids = append(ids, id)
	}
	m.mu.RUnlock()

	for _, id := range ids {
		if err := m.Stop(ctx, id); err != nil {
			ml.Warn("stop all: session stop failed", "workspace", id, "err", err)
		}
	}
}

// Remove removes a dead session from tracking (called by watchdog).
func (m *Manager) Remove(workspaceID string) {
	m.mu.Lock()
	info, exists := m.sessions[workspaceID]
	if exists {
		delete(m.sessions, workspaceID)
	}
	m.mu.Unlock()

	if exists && info.logFile != nil {
		info.logFile.Close()
	}
}

// Subscribe registers a WebSocket client for output broadcast.
// Returns a channel that receives terminal output and a cleanup function.
func (s *SessionInfo) Subscribe() (<-chan []byte, func()) {
	ch := make(chan []byte, 256)
	s.mu.Lock()
	s.clients[ch] = struct{}{}
	s.mu.Unlock()

	return ch, func() {
		s.mu.Lock()
		delete(s.clients, ch)
		s.mu.Unlock()
	}
}

// SendInput writes data to the terminal's stdin.
func (s *SessionInfo) SendInput(data []byte) error {
	_, err := s.term.Write(data)
	return err
}

// readPump reads from the terminal and broadcasts to all subscribers + log file.
func (s *SessionInfo) readPump() {
	defer close(s.done)
	buf := make([]byte, 4096)
	for {
		n, err := s.term.Read(buf)
		if n > 0 {
			data := make([]byte, n)
			copy(data, buf[:n])

			s.ring.Write(data)

			if s.logFile != nil {
				s.logFile.Write(data)
			}

			s.mu.Lock()
			for ch := range s.clients {
				select {
				case ch <- data:
				default:
					// Slow client — drop frame.
				}
			}
			s.mu.Unlock()
		}
		if err != nil {
			if err != io.EOF {
				ml.Debug("terminal read ended", "workspace", s.WorkspaceID, "err", err)
			}
			return
		}
	}
}
