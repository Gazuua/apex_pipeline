// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "net/http"

func (s *Server) handleDashboard(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		s.renderError(w, http.StatusNotFound, "not found")
		return
	}

	data := map[string]any{"Page": "dashboard"}

	if s.store != nil {
		summary, err := queryDashboardSummary(s.store)
		if err != nil {
			s.renderHTMXError(w, "dashboard query failed: "+err.Error())
			return
		}
		data["Summary"] = summary

		branches, _ := queryActiveBranches(s.store)
		data["Branches"] = branches

		queue, _ := queryQueueStatus(s.store)
		data["Queue"] = queue

		history, _ := queryBranchHistory(s.store, 10)
		data["History"] = history
	}

	s.renderPage(w, "dashboard", data)
}

func (s *Server) handlePartialSummary(w http.ResponseWriter, _ *http.Request) {
	data := map[string]any{}
	if s.store != nil {
		summary, err := queryDashboardSummary(s.store)
		if err != nil {
			s.renderHTMXError(w, err.Error())
			return
		}
		data["Summary"] = summary
	}
	s.renderPartial(w, "partial-summary", data)
}

func (s *Server) handlePartialActiveBranches(w http.ResponseWriter, _ *http.Request) {
	data := map[string]any{}
	if s.store != nil {
		branches, err := queryActiveBranches(s.store)
		if err != nil {
			s.renderHTMXError(w, err.Error())
			return
		}
		data["Branches"] = branches
	}
	s.renderPartial(w, "partial-active-branches", data)
}

func (s *Server) handlePartialQueueStatus(w http.ResponseWriter, _ *http.Request) {
	data := map[string]any{}
	if s.store != nil {
		queue, err := queryQueueStatus(s.store)
		if err != nil {
			s.renderHTMXError(w, err.Error())
			return
		}
		data["Queue"] = queue
	}
	s.renderPartial(w, "partial-queue-status", data)
}

func (s *Server) handlePartialRecentHistory(w http.ResponseWriter, _ *http.Request) {
	data := map[string]any{}
	if s.store != nil {
		history, err := queryBranchHistory(s.store, 10)
		if err != nil {
			s.renderHTMXError(w, err.Error())
			return
		}
		data["History"] = history
	}
	s.renderPartial(w, "partial-recent-history", data)
}
