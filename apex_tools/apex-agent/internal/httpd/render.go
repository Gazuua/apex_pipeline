// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"embed"
	"encoding/json"
	"fmt"
	"html/template"
	"io/fs"
	"net/http"
	"net/url"
)

//go:embed templates/*.html templates/partials/*.html
var templateFS embed.FS

//go:embed static/*
var staticFS embed.FS

// pageFiles maps page names to their template files.
// Each page gets layout.html + partials + its own page template.
var pageFiles = map[string]string{
	"dashboard": "templates/dashboard.html",
	"backlog":   "templates/backlog.html",
	"handoff":   "templates/handoff.html",
	"queue":     "templates/queue.html",
	"branches":  "templates/branches.html",
}

// loadAllPages builds a per-page template set. Each set includes
// layout.html, all partials, and one page template — avoiding the
// "content" block name collision when all pages are parsed together.
func loadAllPages() (map[string]*template.Template, error) {
	funcMap := template.FuncMap{
		"add": func(a, b int) int { return a + b },
		"historyUrl": func(channel string, offset, limit int, from, to string) string {
			u := fmt.Sprintf("/partials/queue-history?channel=%s&offset=%d&limit=%d", url.QueryEscape(channel), offset, limit)
			if from != "" {
				u += "&from=" + url.QueryEscape(from)
			}
			if to != "" {
				u += "&to=" + url.QueryEscape(to)
			}
			return u
		},
	}
	// Parse partials first as a shared base.
	partials, err := template.New("").Funcs(funcMap).ParseFS(templateFS, "templates/partials/*.html")
	if err != nil {
		return nil, fmt.Errorf("parse partials: %w", err)
	}

	pages := make(map[string]*template.Template, len(pageFiles))
	for name, file := range pageFiles {
		// Clone partials → add layout → add page template.
		t, err := partials.Clone()
		if err != nil {
			return nil, fmt.Errorf("clone partials for %s: %w", name, err)
		}
		t, err = t.ParseFS(templateFS, "templates/layout.html", file)
		if err != nil {
			return nil, fmt.Errorf("parse page %s: %w", name, err)
		}
		pages[name] = t
	}

	// Also keep the partials-only set for HTMX partial rendering.
	pages["_partials"] = partials

	return pages, nil
}

// InitTemplates loads embedded HTML templates. Call after template files exist.
func (s *Server) InitTemplates() error {
	pages, err := loadAllPages()
	if err != nil {
		return err
	}
	s.pages = pages
	return nil
}

// StaticSubFS returns a http.FileSystem for serving embedded static files.
func StaticSubFS() http.FileSystem {
	sub, _ := fs.Sub(staticFS, "static")
	return http.FS(sub)
}

// renderPage renders a full page (layout + content).
func (s *Server) renderPage(w http.ResponseWriter, page string, data any) {
	if s.pages == nil {
		s.renderHTMXError(w, "templates not loaded")
		return
	}
	tmpl, ok := s.pages[page]
	if !ok {
		http.Error(w, "page not found: "+page, http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, "layout", data); err != nil {
		ml.Error("template render failed", "page", page, "err", err)
		http.Error(w, "internal error", http.StatusInternalServerError)
	}
}

// renderPartial renders a named partial template (for HTMX swap).
func (s *Server) renderPartial(w http.ResponseWriter, name string, data any) {
	if s.pages == nil {
		s.renderHTMXError(w, "templates not loaded")
		return
	}
	tmpl := s.pages["_partials"]
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, name, data); err != nil {
		ml.Error("partial render failed", "name", name, "err", err)
		http.Error(w, "internal error", http.StatusInternalServerError)
	}
}

func (s *Server) renderJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data) //nolint:errcheck
}

func (s *Server) renderError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": msg}) //nolint:errcheck
}

func (s *Server) renderHTMXError(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusInternalServerError)
	fmt.Fprintf(w, `<div class="error-banner">⚠ %s</div>`, template.HTMLEscapeString(msg))
}
