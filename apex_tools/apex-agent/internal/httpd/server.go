// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"context"
	"encoding/json"
	"html/template"
	"net"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Dispatcher routes requests to module handlers (satisfied by daemon.Router).
type Dispatcher interface {
	Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error)
}

// Server is the HTTP dashboard server embedded in the daemon.
type Server struct {
	store       *store.Store
	router      Dispatcher
	httpSrv     *http.Server
	listener    net.Listener
	lastRequest atomic.Int64
	pages       map[string]*template.Template
	addr        string
}

// New creates a new HTTP server. store and router may be nil for health-only mode.
func New(st *store.Store, router Dispatcher, addr string) *Server {
	s := &Server{
		store:  st,
		router: router,
		addr:   addr,
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.handleHealth)
	mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServer(StaticSubFS())))

	// Load templates (non-fatal — health/static still work without them).
	if err := s.InitTemplates(); err != nil {
		// Templates will be nil; renderPage/renderPartial return error banners.
		_ = err
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
	go s.httpSrv.Serve(ln) //nolint:errcheck // closed via Shutdown
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
