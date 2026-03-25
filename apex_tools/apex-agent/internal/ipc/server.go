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

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/dispatch"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("ipc")

// Server accepts IPC connections and dispatches requests to the router.
type Server struct {
	addr        string
	router      dispatch.Dispatcher
	listener    net.Listener
	lastRequest atomic.Int64
	wg          sync.WaitGroup
}

// NewServer creates a new IPC server with the given address and dispatcher.
func NewServer(addr string, router dispatch.Dispatcher) *Server {
	return &Server{addr: addr, router: router}
}

// LastRequestTime returns the Unix timestamp of the most recent request.
func (s *Server) LastRequestTime() int64 {
	return s.lastRequest.Load()
}

// Serve starts the IPC listener and processes incoming connections until ctx is cancelled.
func (s *Server) Serve(ctx context.Context) error {
	ml.Info("IPC server starting", "addr", s.addr)
	ln, err := Listen(s.addr)
	if err != nil {
		ml.Error("IPC listen failed", "addr", s.addr, "err", err)
		return err
	}
	s.listener = ln
	ml.Info("IPC server listening", "addr", s.addr)

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
				ml.Warn("accept error", "err", err)
				time.Sleep(50 * time.Millisecond) // prevent tight loop on persistent errors
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

	// Read deadline — protect against slow/stalled clients
	conn.SetReadDeadline(time.Now().Add(30 * time.Second))

	var req Request
	if err := ReadMessage(conn, &req); err != nil {
		ml.Error("read request failed", "err", err)
		resp := Response{OK: false, Error: err.Error()}
		if wErr := WriteMessage(conn, &resp); wErr != nil {
			ml.Error("write read-error response failed", "err", wErr)
		}
		return
	}

	// Clear deadline for handler execution — handlers may block (e.g., queue.Acquire 30min)
	conn.SetDeadline(time.Time{})

	s.lastRequest.Store(time.Now().Unix())
	ml.Debug("request received", "module", req.Module, "action", req.Action, "workspace", req.Workspace)

	// Router returns (any, error) — wrap into ipc.Response
	startTime := time.Now()
	result, err := s.router.Dispatch(ctx, req.Module, req.Action, req.Params, req.Workspace)
	elapsed := time.Since(startTime)

	// Write deadline — protect against slow/stalled clients on response
	conn.SetWriteDeadline(time.Now().Add(30 * time.Second))

	if err != nil {
		ml.Warn("request failed", "module", req.Module, "action", req.Action, "err", err, "elapsed_ms", elapsed.Milliseconds())
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

	ml.Debug("response sent", "module", req.Module, "action", req.Action, "ok", true, "elapsed_ms", elapsed.Milliseconds())
	if wErr := WriteMessage(conn, &Response{OK: true, Data: data}); wErr != nil {
		ml.Error("write response failed", "module", req.Module, "action", req.Action, "err", wErr)
	}
}
