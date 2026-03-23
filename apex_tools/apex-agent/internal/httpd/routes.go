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

	// Queue partial (full page polling)
	mux.HandleFunc("GET /partials/queue-page", s.handlePartialQueuePage)

	// Backlog inline detail (for handoff page)
	mux.HandleFunc("GET /partials/backlog-inline/{id}", s.handlePartialBacklogInline)

	// JSON API
	mux.HandleFunc("GET /api/backlog", s.handleAPIBacklog)
	mux.HandleFunc("GET /api/handoff", s.handleAPIHandoff)
	mux.HandleFunc("GET /api/queue", s.handleAPIQueue)
}
