// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "net/http"

func (s *Server) handleQueue(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{"Page": "queue"}

	if s.store != nil {
		entries, err := queryQueueStatus(s.store)
		if err != nil {
			s.renderHTMXError(w, "queue query: "+err.Error())
			return
		}
		data["Entries"] = entries
	}

	s.renderPage(w, "queue", data)
}

func (s *Server) handleAPIQueue(w http.ResponseWriter, r *http.Request) {
	if s.store == nil {
		s.renderError(w, http.StatusServiceUnavailable, "store not available")
		return
	}
	entries, err := queryQueueStatus(s.store)
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, map[string]any{"ok": true, "data": entries})
}
