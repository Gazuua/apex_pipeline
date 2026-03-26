// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "net/http"

func (s *Server) registerRoutes(mux *http.ServeMux) {
	// Pages
	mux.HandleFunc("GET /", s.handleDashboard)
	mux.HandleFunc("GET /backlog", s.handleBacklog)
	mux.HandleFunc("GET /handoff", s.handleHandoff)
	mux.HandleFunc("GET /queue", s.handleQueue)

	// Dashboard partials (HTMX polling)
	mux.HandleFunc("GET /partials/summary", s.handlePartialSummary)
	mux.HandleFunc("GET /partials/active-branches", s.handlePartialActiveBranches)
	mux.HandleFunc("GET /partials/queue-status", s.handlePartialQueueStatus)
	mux.HandleFunc("GET /partials/recent-history", s.handlePartialRecentHistory)

	// Backlog partials
	mux.HandleFunc("GET /partials/backlog-table", s.handlePartialBacklogTable)

	// Queue history partial (infinite scroll + filter)
	mux.HandleFunc("GET /partials/queue-history", s.handlePartialQueueHistory)

	// Backlog inline detail (for handoff page)
	mux.HandleFunc("GET /partials/backlog-inline/{id}", s.handlePartialBacklogInline)

	// Branches page + partials
	mux.HandleFunc("GET /branches", s.handleBranches)
	mux.HandleFunc("GET /partials/branches", s.handlePartialBranches)
	mux.HandleFunc("GET /partials/blocked-badge", s.handlePartialBlockedBadge)

	// JSON API
	mux.HandleFunc("GET /api/backlog", s.handleAPIBacklog)
	mux.HandleFunc("GET /api/handoff", s.handleAPIHandoff)
	mux.HandleFunc("GET /api/queue", s.handleAPIQueue)

	// Workspace API
	mux.HandleFunc("GET /api/workspace", s.handleAPIWorkspace)
	mux.HandleFunc("POST /api/workspace/scan", s.handleWorkspaceScan)
	mux.HandleFunc("POST /api/workspace/{id}/sync", s.handleWorkspaceSync)
	mux.HandleFunc("POST /api/workspace/sync-all", s.handleWorkspaceSyncAll)

	// Session proxy routes (→ session server).
	// No method restriction: the session server handles method dispatch internally.
	if s.sessionProxy != nil {
		mux.HandleFunc("/api/session/", func(w http.ResponseWriter, r *http.Request) {
			s.sessionProxy.HandleHTTP(w, r)
		})
		mux.HandleFunc("/ws/session/", func(w http.ResponseWriter, r *http.Request) {
			if IsWebSocket(r) {
				s.sessionProxy.HandleWebSocket(w, r)
			} else {
				s.sessionProxy.HandleHTTP(w, r)
			}
		})
	}
}
