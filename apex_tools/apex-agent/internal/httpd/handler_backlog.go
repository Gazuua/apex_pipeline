// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "net/http"

func (s *Server) handleBacklog(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{"Page": "backlog"}

	if s.store != nil {
		items, err := queryBacklogList(s.store, parseBacklogFilter(r))
		if err != nil {
			s.renderHTMXError(w, "backlog query: "+err.Error())
			return
		}
		data["Items"] = items
	}

	s.renderPage(w, "backlog", data)
}

func (s *Server) handlePartialBacklogTable(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{}
	if s.store != nil {
		items, err := queryBacklogList(s.store, parseBacklogFilter(r))
		if err != nil {
			s.renderHTMXError(w, err.Error())
			return
		}
		data["Items"] = items
	}
	s.renderPartial(w, "partial-backlog-table", data)
}

func (s *Server) handleAPIBacklog(w http.ResponseWriter, r *http.Request) {
	if s.store == nil {
		s.renderError(w, http.StatusServiceUnavailable, "store not available")
		return
	}
	items, err := queryBacklogList(s.store, parseBacklogFilter(r))
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, map[string]any{"ok": true, "data": items})
}

func parseBacklogFilter(r *http.Request) BacklogFilter {
	q := r.URL.Query()
	return BacklogFilter{
		Status:    q.Get("status"),
		Severity:  q.Get("severity"),
		Timeframe: q.Get("timeframe"),
		Scope:     q.Get("scope"),
		Type:      q.Get("type"),
		SortBy:    q.Get("sort"),
		SortDir:   q.Get("dir"),
	}
}
