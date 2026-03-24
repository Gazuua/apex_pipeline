// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"html/template"
	"net"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/dispatch"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("httpd")

// BacklogQuerier abstracts backlog module queries for the dashboard.
// Implemented by adapter in daemon_cmd.go to avoid import cycles.
type BacklogQuerier interface {
	DashboardStatusCounts() (map[string]int, error)
	DashboardSeverityCounts() (map[string]int, error)
	DashboardListItems(filter BacklogFilter) ([]BacklogItem, error)
	DashboardGetItemByID(id int) (*BacklogItem, error)
}

// HandoffQuerier abstracts handoff module queries for the dashboard.
// Implemented by adapter in daemon_cmd.go to avoid import cycles.
type HandoffQuerier interface {
	DashboardActiveBranchesList() ([]ActiveBranch, error)
	DashboardActiveCount() (int, error)
	DashboardBranchHistoryList(limit int) ([]BranchHistory, error)
}

// QueueQuerier abstracts queue module queries for the dashboard.
// Implemented by adapter in daemon_cmd.go to avoid import cycles.
type QueueQuerier interface {
	DashboardQueueAll() ([]QueueEntry, error)
	DashboardLockStatus(channel string) (bool, error)
	DashboardQueueHistory(channel string, offset, limit int, from, to string) ([]QueueHistoryEntry, error)
}

// Server is the HTTP dashboard server embedded in the daemon.
type Server struct {
	backlogMgr  BacklogQuerier
	handoffMgr  HandoffQuerier
	queueMgr    QueueQuerier
	router      dispatch.Dispatcher
	httpSrv     *http.Server
	listener    net.Listener
	lastRequest atomic.Int64
	pages       map[string]*template.Template
	addr        string
}

// New creates a new HTTP server.
// backlogMgr, handoffMgr, queueMgr may be nil for health-only mode.
func New(backlogMgr BacklogQuerier, handoffMgr HandoffQuerier, queueMgr QueueQuerier, router dispatch.Dispatcher, addr string) *Server {
	s := &Server{
		backlogMgr: backlogMgr,
		handoffMgr: handoffMgr,
		queueMgr:   queueMgr,
		router:     router,
		addr:       addr,
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.handleHealth)
	mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServer(StaticSubFS())))

	// Register page/partial/API routes.
	s.registerRoutes(mux)

	// Load templates (non-fatal — health/static still work without them).
	if err := s.InitTemplates(); err != nil {
		ml.Warn("template init failed, dashboard pages unavailable", "err", err)
	}

	s.httpSrv = &http.Server{
		Handler:      s.idleResetMiddleware(mux),
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 30 * time.Second,
	}
	return s
}

// Start begins listening. Uses the configured address; port 0 picks a random port.
func (s *Server) Start() error {
	ln, err := net.Listen("tcp", s.addr)
	if err != nil {
		return err
	}
	s.listener = ln
	s.addr = ln.Addr().String()
	go func() {
		if err := s.httpSrv.Serve(ln); err != nil && err != http.ErrServerClosed {
			ml.Warn("HTTP serve ended unexpectedly", "err", err)
		}
	}()
	return nil
}

// Stop gracefully shuts down the HTTP server with a 5-second timeout.
func (s *Server) Stop() error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return s.httpSrv.Shutdown(ctx)
}

// Addr returns the actual bound address (useful when port 0 was used).
func (s *Server) Addr() string {
	return s.addr
}

// LastRequestTime returns the Unix timestamp of the last HTTP request.
func (s *Server) LastRequestTime() int64 {
	return s.lastRequest.Load()
}

func (s *Server) idleResetMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.lastRequest.Store(time.Now().Unix())
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true,"status":"healthy"}`)) //nolint:errcheck
}
