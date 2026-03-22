// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("ipc")

// Dispatcher routes an IPC request to the appropriate handler.
type Dispatcher interface {
	Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error)
}

// Server accepts IPC connections and dispatches requests to the router.
type Server struct {
	addr        string
	router      Dispatcher
	listener    net.Listener
	lastRequest atomic.Int64
	wg          sync.WaitGroup
}

// NewServer creates a new IPC server with the given address and dispatcher.
func NewServer(addr string, router Dispatcher) *Server {
	return &Server{addr: addr, router: router}
}

// LastRequestTime returns the Unix timestamp of the most recent request.
func (s *Server) LastRequestTime() int64 {
	return s.lastRequest.Load()
}

// Serve starts the IPC listener and processes incoming connections until ctx is cancelled.
func (s *Server) Serve(ctx context.Context) error {
	ln, err := Listen(s.addr)
	if err != nil {
		return err
	}
	s.listener = ln

	go func() {
		<-ctx.Done()
		ln.Close()
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				s.wg.Wait()
				return nil
			default:
				continue
			}
		}
		s.wg.Add(1)
		go s.handleConn(ctx, conn)
	}
}

func (s *Server) handleConn(ctx context.Context, conn net.Conn) {
	defer s.wg.Done()
	defer conn.Close()

	conn.SetDeadline(time.Now().Add(30 * time.Second))

	var req Request
	if err := ReadMessage(conn, &req); err != nil {
		ml.Error("read request failed", "err", err)
		resp := Response{OK: false, Error: err.Error()}
		WriteMessage(conn, &resp)
		return
	}

	s.lastRequest.Store(time.Now().Unix())
	ml.Debug("request received", "module", req.Module, "action", req.Action)

	// Router returns (any, error) — wrap into ipc.Response
	result, err := s.router.Dispatch(ctx, req.Module, req.Action, req.Params, req.Workspace)
	if err != nil {
		ml.Debug("request error", "module", req.Module, "action", req.Action, "err", err)
		if wErr := WriteMessage(conn, &Response{OK: false, Error: err.Error()}); wErr != nil {
			ml.Error("write error response failed", "err", wErr)
		}
		return
	}

	data, err := json.Marshal(result)
	if err != nil {
		ml.Error("marshal result failed", "err", err)
		if wErr := WriteMessage(conn, &Response{OK: false, Error: fmt.Sprintf("marshal: %v", err)}); wErr != nil {
			ml.Error("write marshal-error response failed", "err", wErr)
		}
		return
	}

	ml.Debug("response sent", "module", req.Module, "action", req.Action, "ok", true)
	if wErr := WriteMessage(conn, &Response{OK: true, Data: data}); wErr != nil {
		ml.Error("write response failed", "module", req.Module, "action", req.Action, "err", wErr)
	}
}
