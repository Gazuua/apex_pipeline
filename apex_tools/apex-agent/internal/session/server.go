// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/workspace"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Server is the standalone session server process (:7601).
type Server struct {
	cfg        config.SessionConfig
	mgr        *Manager
	dog        *Watchdog
	wsMgr      *workspace.Manager
	httpSrv    *http.Server
	ln         net.Listener
	shutdownCh chan struct{}
}

// NewServer creates a session server.
func NewServer(cfg config.SessionConfig, wsCfg config.WorkspaceConfig, st *store.Store) *Server {
	logDir := platform.SessionLogDir(cfg.LogDir)
	mgr := NewManager(ManagerConfig{
		OutputBufferLines: cfg.OutputBufferLines,
		LogDir:            logDir,
	})
	wsMgr := workspace.NewManager(st, &wsCfg)

	onUpdate := func(wsID, status string, pid int, sessionID, logPath string) {
		ctx := context.Background()
		wsMgr.UpdateSession(ctx, wsID, workspace.SessionUpdate{
			SessionID:     sessionID,
			SessionPID:    pid,
			SessionStatus: status,
			SessionLog:    logPath,
		})
	}

	dog := NewWatchdog(mgr, cfg.WatchdogInterval, onUpdate)

	return &Server{
		cfg:        cfg,
		mgr:        mgr,
		dog:        dog,
		wsMgr:      wsMgr,
		shutdownCh: make(chan struct{}, 1),
	}
}

// Run starts the HTTP server and watchdog. Blocks until ctx is canceled.
func (s *Server) Run(ctx context.Context) error {
	pidPath := platform.SessionPIDFilePath()
	if err := os.WriteFile(pidPath, []byte(strconv.Itoa(os.Getpid())), 0o600); err != nil {
		return fmt.Errorf("write session PID: %w", err)
	}
	defer os.Remove(pidPath)

	s.cleanZombies(ctx)
	go s.dog.Run(ctx)

	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/sessions", s.handleListSessions)
	mux.HandleFunc("GET /api/session/{id}/status", s.handleSessionStatus)
	mux.HandleFunc("POST /api/session/{id}/start", s.handleStartSession)
	mux.HandleFunc("POST /api/session/{id}/stop", s.handleStopSession)
	mux.HandleFunc("POST /api/session/{id}/send", s.handleSendInput)
	mux.HandleFunc("/ws/session/{id}", s.handleWS)
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		w.Write([]byte(`{"ok":true}`)) //nolint:errcheck
	})
	mux.HandleFunc("POST /api/shutdown", s.handleShutdown)

	ln, err := net.Listen("tcp", s.cfg.Addr)
	if err != nil {
		return fmt.Errorf("session server listen: %w", err)
	}
	s.ln = ln
	s.httpSrv = &http.Server{
		Handler:      mux,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 30 * time.Second,
	}

	ml.Info("session server started", "addr", ln.Addr().String())

	errCh := make(chan error, 1)
	go func() {
		if err := s.httpSrv.Serve(ln); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
		close(errCh)
	}()

	select {
	case <-ctx.Done():
		ml.Info("shutdown: context canceled")
	case <-s.shutdownCh:
		ml.Info("shutdown: requested via API")
	case err := <-errCh:
		return err
	}

	ml.Info("session server shutting down")

	shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	s.mgr.StopAll(shutCtx)

	return s.httpSrv.Shutdown(shutCtx)
}

// Addr returns the actual bound address.
func (s *Server) Addr() string {
	if s.ln != nil {
		return s.ln.Addr().String()
	}
	return s.cfg.Addr
}

func (s *Server) cleanZombies(ctx context.Context) {
	branches, err := s.wsMgr.List(ctx)
	if err != nil {
		return
	}
	for _, b := range branches {
		if b.SessionStatus == StatusManaged && b.SessionPID > 0 {
			if !platform.IsProcessAlive(b.SessionPID) {
				ml.Info("cleaning zombie session", "workspace", b.WorkspaceID, "stale_pid", b.SessionPID)
				s.wsMgr.UpdateSession(ctx, b.WorkspaceID, workspace.SessionUpdate{
					SessionStatus: StatusStop,
					SessionPID:    0,
				})
			}
		}
	}
}

// --- HTTP Handlers ---

func (s *Server) handleListSessions(w http.ResponseWriter, _ *http.Request) {
	sessions := s.mgr.List()
	type item struct {
		WorkspaceID string `json:"workspace_id"`
		SessionID   string `json:"session_id"`
		Status      string `json:"status"`
		PID         int    `json:"pid"`
		StartedAt   string `json:"started_at"`
		LogPath     string `json:"log_path"`
	}
	result := make([]item, 0, len(sessions))
	for _, sess := range sessions {
		result = append(result, item{
			WorkspaceID: sess.WorkspaceID,
			SessionID:   sess.SessionID,
			Status:      sess.Status,
			PID:         sess.PID,
			StartedAt:   sess.StartedAt.Format(time.RFC3339),
			LogPath:     sess.LogPath,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"sessions": result})
}

func (s *Server) handleSessionStatus(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	info := s.mgr.Get(id)
	if info == nil {
		writeJSON(w, http.StatusOK, map[string]any{"workspace_id": id, "status": StatusStop})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"workspace_id": id,
		"status":       info.Status,
		"session_id":   info.SessionID,
		"pid":          info.PID,
		"started_at":   info.StartedAt.Format(time.RFC3339),
		"log_path":     info.LogPath,
	})
}

func (s *Server) handleStartSession(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")

	if existing := s.mgr.Get(wsID); existing != nil {
		writeJSON(w, http.StatusConflict, map[string]any{"ok": false, "error": "session already active"})
		return
	}

	mode := r.URL.Query().Get("mode")
	if mode == "" {
		mode = "new"
	}

	ctx := r.Context()
	branch, err := s.wsMgr.Get(ctx, wsID)
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]any{"ok": false, "error": "workspace not found: " + wsID})
		return
	}

	cmdLine := "claude --dangerously-skip-permissions"
	var sessionID string
	if mode == "resume" && branch.SessionID != "" {
		sessionID = branch.SessionID
		cmdLine += " --resume " + branch.SessionID
	} else {
		sessionID = fmt.Sprintf("%s-%d", wsID, time.Now().Unix())
	}

	term, err := NewConPTY(cmdLine, 120, 40)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": "spawn terminal: " + err.Error()})
		return
	}

	sess, err := s.mgr.Register(wsID, sessionID, term)
	if err != nil {
		term.Close()
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": err.Error()})
		return
	}

	s.wsMgr.UpdateSession(ctx, wsID, workspace.SessionUpdate{
		SessionID:     sessionID,
		SessionPID:    term.Pid(),
		SessionStatus: StatusManaged,
		SessionLog:    sess.LogPath,
	})

	writeJSON(w, http.StatusOK, map[string]any{
		"ok":         true,
		"session_id": sessionID,
		"pid":        term.Pid(),
	})
}

func (s *Server) handleStopSession(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	if err := s.mgr.Stop(r.Context(), wsID); err != nil {
		writeJSON(w, http.StatusNotFound, map[string]any{"ok": false, "error": err.Error()})
		return
	}

	s.wsMgr.UpdateSession(r.Context(), wsID, workspace.SessionUpdate{
		SessionStatus: StatusStop,
		SessionPID:    0,
	})

	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleSendInput(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	info := s.mgr.Get(wsID)
	if info == nil {
		writeJSON(w, http.StatusNotFound, map[string]any{"ok": false, "error": "no active session"})
		return
	}

	var body struct {
		Text string `json:"text"`
	}
	// Limit body to 1MB to prevent unbounded memory allocation.
	if err := json.NewDecoder(io.LimitReader(r.Body, 1<<20)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"ok": false, "error": "invalid body"})
		return
	}

	text := body.Text
	if !strings.HasSuffix(text, "\n") {
		text += "\n"
	}

	if err := info.SendInput([]byte(text)); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "text": text})
}

func (s *Server) handleShutdown(w http.ResponseWriter, _ *http.Request) {
	select {
	case s.shutdownCh <- struct{}{}:
		ml.Audit("shutdown requested via HTTP API")
		writeJSON(w, http.StatusOK, map[string]any{"ok": true, "status": "shutting_down"})
	default:
		writeJSON(w, http.StatusOK, map[string]any{"ok": true, "status": "already_shutting_down"})
	}
}

func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	HandleWebSocket(s.mgr, w, r, wsID)
}

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data) //nolint:errcheck
}
