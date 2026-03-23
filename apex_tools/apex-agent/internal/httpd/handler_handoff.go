// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "net/http"

func (s *Server) handleHandoff(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{"Page": "handoff"}

	if s.store != nil {
		branches, err := queryActiveBranches(s.store)
		if err != nil {
			s.renderHTMXError(w, "handoff query: "+err.Error())
			return
		}
		data["Branches"] = branches

		history, _ := queryBranchHistory(s.store, 20)
		data["History"] = history
	}

	s.renderPage(w, "handoff", data)
}

func (s *Server) handleAPIHandoff(w http.ResponseWriter, r *http.Request) {
	if s.store == nil {
		s.renderError(w, http.StatusServiceUnavailable, "store not available")
		return
	}
	branches, err := queryActiveBranches(s.store)
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	history, _ := queryBranchHistory(s.store, 20)
	s.renderJSON(w, http.StatusOK, map[string]any{
		"ok":   true,
		"data": map[string]any{"branches": branches, "history": history},
	})
}
