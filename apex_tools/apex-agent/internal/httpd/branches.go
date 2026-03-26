// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"encoding/json"
	"net/http"
)

func (s *Server) handleBranches(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{
		"Page": "branches",
	}
	if s.workspaceMgr != nil {
		branches, err := s.workspaceMgr.DashboardBranchesList()
		if err != nil {
			ml.Warn("branches page: list failed", "err", err)
		}
		data["Branches"] = branches
	}
	s.renderPage(w, "branches", data)
}

func (s *Server) handlePartialBranches(w http.ResponseWriter, r *http.Request) {
	if s.workspaceMgr == nil {
		s.renderHTMXError(w, "workspace not available")
		return
	}
	branches, err := s.workspaceMgr.DashboardBranchesList()
	if err != nil {
		s.renderHTMXError(w, err.Error())
		return
	}
	s.renderPartial(w, "branches-list", branches)
}

func (s *Server) handlePartialBlockedBadge(w http.ResponseWriter, r *http.Request) {
	count := 0
	if s.workspaceMgr != nil {
		count, _ = s.workspaceMgr.DashboardBlockedCount()
	}
	s.renderPartial(w, "blocked-badge", map[string]int{"Count": count})
}

// --- Workspace JSON API (proxied via daemon IPC router) ---

func (s *Server) handleAPIWorkspace(w http.ResponseWriter, r *http.Request) {
	if s.workspaceMgr == nil {
		s.renderError(w, http.StatusServiceUnavailable, "workspace not available")
		return
	}
	branches, err := s.workspaceMgr.DashboardBranchesList()
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, branches)
}

func (s *Server) handleWorkspaceScan(w http.ResponseWriter, r *http.Request) {
	if s.router == nil {
		s.renderError(w, http.StatusServiceUnavailable, "router not available")
		return
	}
	result, err := s.router.Dispatch(r.Context(), "workspace", "scan", nil, "")
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, result)
}

func (s *Server) handleWorkspaceSync(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	if s.router == nil {
		s.renderError(w, http.StatusServiceUnavailable, "router not available")
		return
	}
	params, _ := json.Marshal(map[string]string{"workspace_id": wsID})
	result, err := s.router.Dispatch(r.Context(), "workspace", "sync", params, "")
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, result)
}

func (s *Server) handleWorkspaceSyncAll(w http.ResponseWriter, r *http.Request) {
	if s.router == nil {
		s.renderError(w, http.StatusServiceUnavailable, "router not available")
		return
	}
	params, _ := json.Marshal(map[string]any{"all": true})
	result, err := s.router.Dispatch(r.Context(), "workspace", "sync", params, "")
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, result)
}
