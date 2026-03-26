// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// SessionProxy forwards /api/session/* and /ws/session/* to the session server.
type SessionProxy struct {
	target   *url.URL
	httpRP   *httputil.ReverseProxy
	wsDialer *websocket.Dialer
}

// NewSessionProxy creates a reverse proxy to the session server.
// Panics if targetAddr is not a valid host:port — caller must validate.
func NewSessionProxy(targetAddr string) *SessionProxy {
	target, err := url.Parse("http://" + targetAddr)
	if err != nil {
		panic("invalid session server address: " + err.Error())
	}
	httpRP := httputil.NewSingleHostReverseProxy(target)
	httpRP.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		ml.Debug("session proxy error", "path", r.URL.Path, "err", err)
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadGateway)
		w.Write([]byte(`{"ok":false,"error":"session server unavailable"}`)) //nolint:errcheck
	}

	return &SessionProxy{
		target: target,
		httpRP: httpRP,
		wsDialer: &websocket.Dialer{
			HandshakeTimeout: 10 * time.Second,
		},
	}
}

// HandleHTTP proxies regular HTTP requests to the session server.
func (p *SessionProxy) HandleHTTP(w http.ResponseWriter, r *http.Request) {
	p.httpRP.ServeHTTP(w, r)
}

// HandleWebSocket proxies a WebSocket connection to the session server.
func (p *SessionProxy) HandleWebSocket(w http.ResponseWriter, r *http.Request) {
	backendURL := "ws://" + p.target.Host + r.URL.Path
	if r.URL.RawQuery != "" {
		backendURL += "?" + r.URL.RawQuery
	}

	backendConn, resp, err := p.wsDialer.Dial(backendURL, nil)
	if err != nil {
		ml.Debug("ws proxy dial failed", "url", backendURL, "err", err)
		status := http.StatusBadGateway
		if resp != nil {
			status = resp.StatusCode
		}
		http.Error(w, "session server unavailable", status)
		return
	}
	defer backendConn.Close()

	upgrader := websocket.Upgrader{
		ReadBufferSize:  4096,
		WriteBufferSize: 4096,
		CheckOrigin:     func(r *http.Request) bool { return true },
	}
	clientConn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer clientConn.Close()

	// Limit incoming message sizes to prevent memory exhaustion.
	clientConn.SetReadLimit(64 * 1024)
	backendConn.SetReadLimit(1024 * 1024) // backend output can be larger (terminal output)

	// Bidirectional pipe.
	done := make(chan struct{})

	// Backend → Client
	go func() {
		defer close(done)
		for {
			mt, msg, err := backendConn.ReadMessage()
			if err != nil {
				return
			}
			if err := clientConn.WriteMessage(mt, msg); err != nil {
				return
			}
		}
	}()

	// Client → Backend
	for {
		mt, msg, err := clientConn.ReadMessage()
		if err != nil {
			break
		}
		if err := backendConn.WriteMessage(mt, msg); err != nil {
			break
		}
	}

	// Close backend to unblock the backend→client goroutine.
	backendConn.Close()
	<-done
}

// IsWebSocket checks if a request is a WebSocket upgrade request.
func IsWebSocket(r *http.Request) bool {
	return strings.EqualFold(r.Header.Get("Upgrade"), "websocket")
}
