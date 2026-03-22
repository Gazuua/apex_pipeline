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

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
)

// Server accepts IPC connections and dispatches requests to the router.
type Server struct {
	addr        string
	router      *daemon.Router
	listener    net.Listener
	lastRequest atomic.Int64
	wg          sync.WaitGroup
}

// NewServer creates a new IPC server with the given address and router.
func NewServer(addr string, router *daemon.Router) *Server {
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

	var req Request
	if err := ReadMessage(conn, &req); err != nil {
		resp := Response{OK: false, Error: err.Error()}
		WriteMessage(conn, &resp)
		return
	}

	s.lastRequest.Store(time.Now().Unix())

	// Router returns (any, error) — wrap into ipc.Response
	result, err := s.router.Dispatch(ctx, req.Module, req.Action, req.Params, req.Workspace)
	if err != nil {
		WriteMessage(conn, &Response{OK: false, Error: err.Error()})
		return
	}

	data, err := json.Marshal(result)
	if err != nil {
		WriteMessage(conn, &Response{OK: false, Error: fmt.Sprintf("marshal: %v", err)})
		return
	}

	WriteMessage(conn, &Response{OK: true, Data: data})
}
