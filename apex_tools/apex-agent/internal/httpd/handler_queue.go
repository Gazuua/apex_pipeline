// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"fmt"
	"net/http"
)

func (s *Server) handleQueue(w http.ResponseWriter, r *http.Request) {
	s.renderPage(w, "queue", map[string]any{"Page": "queue"})
}

func (s *Server) handlePartialQueueHistory(w http.ResponseWriter, r *http.Request) {
	if s.queueMgr == nil {
		s.renderHTMXError(w, "queue not available")
		return
	}

	channel := r.URL.Query().Get("channel")
	if channel == "" {
		channel = "build"
	}

	offset := 0
	if v := r.URL.Query().Get("offset"); v != "" {
		fmt.Sscanf(v, "%d", &offset)
	}
	if offset < 0 {
		offset = 0
	}

	limit := 50
	if v := r.URL.Query().Get("limit"); v != "" {
		fmt.Sscanf(v, "%d", &limit)
	}
	if limit < 1 {
		limit = 50
	}
	if limit > 500 {
		limit = 500
	}

	from := r.URL.Query().Get("from")
	to := r.URL.Query().Get("to")

	entries, err := queryQueueHistory(s.queueMgr, channel, offset, limit, from, to)
	if err != nil {
		s.renderHTMXError(w, err.Error())
		return
	}

	data := map[string]any{
		"Entries": entries,
		"Channel": channel,
		"Offset":  offset,
		"Limit":   limit,
		"From":    from,
		"To":      to,
		"HasMore": len(entries) == limit,
	}
	s.renderPartial(w, "partial-queue-history", data)
}

func (s *Server) handleAPIQueue(w http.ResponseWriter, r *http.Request) {
	if s.queueMgr == nil {
		s.renderError(w, http.StatusServiceUnavailable, "store not available")
		return
	}
	entries, err := queryQueueStatus(s.queueMgr)
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, map[string]any{"ok": true, "data": entries})
}
