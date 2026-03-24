// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"fmt"
	"html/template"
	"net/http"
)

func (s *Server) handleBacklog(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{"Page": "backlog"}

	if s.backlogMgr != nil {
		items, err := queryBacklogList(s.backlogMgr, parseBacklogFilter(r))
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
	if s.backlogMgr != nil {
		items, err := queryBacklogList(s.backlogMgr, parseBacklogFilter(r))
		if err != nil {
			s.renderHTMXError(w, err.Error())
			return
		}
		data["Items"] = items
	}
	s.renderPartial(w, "partial-backlog-table", data)
}

func (s *Server) handleAPIBacklog(w http.ResponseWriter, r *http.Request) {
	if s.backlogMgr == nil {
		s.renderError(w, http.StatusServiceUnavailable, "store not available")
		return
	}
	items, err := queryBacklogList(s.backlogMgr, parseBacklogFilter(r))
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, map[string]any{"ok": true, "data": items})
}

func (s *Server) handlePartialBacklogInline(w http.ResponseWriter, r *http.Request) {
	idStr := r.PathValue("id")
	if s.backlogMgr == nil || idStr == "" {
		s.renderHTMXError(w, "not available")
		return
	}

	var id int
	if _, err := fmt.Sscanf(idStr, "%d", &id); err != nil {
		s.renderHTMXError(w, "invalid id")
		return
	}

	item, err := s.backlogMgr.DashboardGetItemByID(id)
	if err != nil || item == nil {
		s.renderHTMXError(w, "backlog not found")
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	fmt.Fprintf(w, `<div class="backlog-inline">
		<div class="backlog-inline-title">#%d — %s</div>
		<div><span class="badge %s">%s</span> <span class="badge %s">%s</span> %s</div>
		<div style="margin-top:6px;white-space:pre-wrap">%s</div>
	</div>`,
		item.ID, template.HTMLEscapeString(item.Title),
		severityBadge(item.Severity), template.HTMLEscapeString(item.Severity),
		statusBadge(item.Status), template.HTMLEscapeString(item.Status),
		template.HTMLEscapeString(item.Scope),
		template.HTMLEscapeString(item.Description))
}

func severityBadge(s string) string {
	switch s {
	case "CRITICAL":
		return "badge-red"
	case "MAJOR":
		return "badge-amber"
	default:
		return "badge-gray"
	}
}

func statusBadge(s string) string {
	switch s {
	case "FIXING":
		return "badge-purple"
	case "OPEN":
		return "badge-green"
	default:
		return "badge-blue"
	}
}

func parseBacklogFilter(r *http.Request) BacklogFilter {
	q := r.URL.Query()
	return BacklogFilter{
		Status:    q["status"],
		Severity:  q["severity"],
		Timeframe: q["timeframe"],
		Scope:     q["scope"],
		Type:      q["type"],
		SortBy:    q.Get("sort"),
		SortDir:   q.Get("dir"),
	}
}
