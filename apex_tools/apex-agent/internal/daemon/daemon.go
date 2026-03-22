// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/version"
)

var ml = log.WithModule("daemon")

type Config struct {
	DBPath      string
	PIDFilePath string
	SocketAddr  string
	IdleTimeout time.Duration
}

type Daemon struct {
	cfg        Config
	store      *store.Store
	router     *Router
	server     *ipc.Server
	modules    []Module
	shutdownCh chan struct{}
}

func New(cfg Config) (*Daemon, error) {
	s, err := store.Open(cfg.DBPath)
	if err != nil {
		return nil, fmt.Errorf("open store: %w", err)
	}

	router := NewRouter()

	return &Daemon{
		cfg:        cfg,
		store:      s,
		router:     router,
		shutdownCh: make(chan struct{}, 1),
	}, nil
}

func (d *Daemon) Register(m Module) {
	d.modules = append(d.modules, m)
}

// Store returns the daemon's underlying data store.
func (d *Daemon) Store() *store.Store { return d.store }

func (d *Daemon) Run(ctx context.Context) error {
	// 1. Write PID file.
	if err := d.writePID(); err != nil {
		return err
	}
	defer d.removePID()

	// 2. Run migrations.
	migrator := store.NewMigrator(d.store)
	for _, m := range d.modules {
		m.RegisterSchema(migrator)
	}
	if err := migrator.Migrate(); err != nil {
		return fmt.Errorf("migrate: %w", err)
	}

	// 3. Register built-in daemon routes.
	d.router.RegisterModule("daemon", func(reg RouteRegistrar) {
		reg.Handle("version", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"version": version.Version}, nil
		})
		reg.Handle("shutdown", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			ml.Audit("shutdown requested via IPC")
			select {
			case d.shutdownCh <- struct{}{}:
			default:
			}
			return map[string]string{"status": "shutting_down"}, nil
		})
	})

	// Register user module routes.
	for _, m := range d.modules {
		mod := m // capture for closure
		d.router.RegisterModule(mod.Name(), func(reg RouteRegistrar) {
			mod.RegisterRoutes(reg)
		})
	}

	// 4. Start modules (with rollback on partial failure).
	var started []Module
	for _, m := range d.modules {
		if err := m.OnStart(ctx); err != nil {
			// Rollback: stop already-started modules in reverse order.
			for i := len(started) - 1; i >= 0; i-- {
				started[i].OnStop()
			}
			return fmt.Errorf("start module %s: %w", m.Name(), err)
		}
		started = append(started, m)
	}

	// 5. Start IPC server.
	d.server = ipc.NewServer(d.cfg.SocketAddr, d.router)
	serverCtx, serverCancel := context.WithCancel(ctx)
	defer serverCancel()

	serverDone := make(chan error, 1)
	go func() { serverDone <- d.server.Serve(serverCtx) }()

	ml.Info("daemon started",
		"pid", os.Getpid(),
		"socket", d.cfg.SocketAddr,
	)

	// 6. Initialize idle timer baseline.
	startTime := time.Now().Unix()

	// 7. Wait for shutdown signal or idle timeout.
	idleTicker := time.NewTicker(30 * time.Second)
	defer idleTicker.Stop()

	for {
		select {
		case <-ctx.Done():
			ml.Info("shutdown requested")
			goto shutdown
		case <-d.shutdownCh:
			ml.Info("shutdown requested via IPC")
			goto shutdown
		case <-idleTicker.C:
			last := d.server.LastRequestTime()
			if last == 0 {
				last = startTime
			}
			if time.Since(time.Unix(last, 0)) > d.cfg.IdleTimeout {
				ml.Info("idle timeout, shutting down")
				goto shutdown
			}
		}
	}

shutdown:
	serverCancel()
	<-serverDone

	// Stop modules in reverse order.
	for i := len(d.modules) - 1; i >= 0; i-- {
		d.modules[i].OnStop()
	}

	d.store.Close()
	return nil
}

func (d *Daemon) writePID() error {
	return os.WriteFile(d.cfg.PIDFilePath, []byte(strconv.Itoa(os.Getpid())), 0o600)
}

func (d *Daemon) removePID() {
	os.Remove(d.cfg.PIDFilePath)
}
