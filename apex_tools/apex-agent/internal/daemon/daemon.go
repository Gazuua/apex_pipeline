// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"sync/atomic"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/httpd"
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
	HTTP        config.HTTPConfig
}

// HTTPServerFactory creates an httpd.Server.
// The factory captures module managers and the daemon's router via closure.
// Set via SetHTTPServerFactory before calling Run.
type HTTPServerFactory func(addr string) *httpd.Server

type Daemon struct {
	cfg              Config
	store            *store.Store
	router           *Router
	server           *ipc.Server
	httpServer       atomic.Pointer[httpd.Server]
	httpFactory      HTTPServerFactory
	modules          []Module
	shutdownCh       chan struct{}
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

// SetHTTPServerFactory sets the factory function for creating the HTTP server.
// Must be called before Run. If not set, daemon uses a nil-manager fallback.
func (d *Daemon) SetHTTPServerFactory(f HTTPServerFactory) {
	d.httpFactory = f
}

// Store returns the daemon's underlying data store.
func (d *Daemon) Store() *store.Store { return d.store }

// Router returns the daemon's request router (implements httpd.Dispatcher).
func (d *Daemon) Router() *Router { return d.router }

// HTTPAddr returns the actual HTTP server address, or "" if not running.
func (d *Daemon) HTTPAddr() string {
	s := d.httpServer.Load()
	if s == nil {
		return ""
	}
	return s.Addr()
}

func (d *Daemon) Run(ctx context.Context) error {
	ml.Info("daemon initializing",
		"pid", os.Getpid(), "db", d.cfg.DBPath, "socket", d.cfg.SocketAddr,
		"http_enabled", d.cfg.HTTP.Enabled)

	// 1. Write PID file.
	if err := d.writePID(); err != nil {
		return err
	}
	defer d.removePID()

	// 2. Run migrations.
	ml.Debug("running database migrations")
	migrator := store.NewMigrator(d.store)
	for _, m := range d.modules {
		m.RegisterSchema(migrator)
	}
	if err := migrator.Migrate(); err != nil {
		ml.Error("migration failed", "err", err)
		return fmt.Errorf("migrate: %w", err)
	}
	ml.Info("database migrations complete")

	// 3. Register built-in daemon routes.
	d.router.RegisterModule("daemon", func(reg RouteRegistrar) {
		reg.Handle("version", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"version": version.Version}, nil
		})
		reg.Handle("shutdown", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			select {
			case d.shutdownCh <- struct{}{}:
				ml.Audit("shutdown requested via IPC")
				return map[string]string{"status": "shutting_down"}, nil
			default:
				ml.Info("duplicate shutdown request ignored")
				return map[string]string{"status": "already_shutting_down"}, nil
			}
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
		ml.Debug("starting module", "module", m.Name())
		if err := m.OnStart(ctx); err != nil {
			ml.Error("module start failed, rolling back", "module", m.Name(), "err", err)
			// Rollback: stop already-started modules in reverse order.
			for i := len(started) - 1; i >= 0; i-- {
				if stopErr := started[i].OnStop(); stopErr != nil {
					ml.Error("rollback module stop failed", "module", started[i].Name(), "err", stopErr)
				}
			}
			return fmt.Errorf("start module %s: %w", m.Name(), err)
		}
		ml.Debug("module started", "module", m.Name())
		started = append(started, m)
	}
	ml.Info("all modules started", "count", len(started))

	// 5. Start IPC server.
	d.server = ipc.NewServer(d.cfg.SocketAddr, d.router)
	serverCtx, serverCancel := context.WithCancel(ctx)
	defer serverCancel()

	serverDone := make(chan error, 1)
	go func() { serverDone <- d.server.Serve(serverCtx) }()

	// 5b. Start HTTP server (optional).
	if d.cfg.HTTP.Enabled && d.cfg.HTTP.Addr != "" {
		var hs *httpd.Server
		if d.httpFactory != nil {
			hs = d.httpFactory(d.cfg.HTTP.Addr)
		} else {
			// Fallback: health-only mode (no module managers)
			hs = httpd.New(nil, nil, nil, nil, d.router, d.cfg.HTTP.Addr, "")
		}
		if err := hs.Start(); err != nil {
			ml.Warn("HTTP server failed to start, dashboard unavailable", "error", err)
		} else {
			d.httpServer.Store(hs)
			ml.Info("HTTP server started", "addr", hs.Addr())
		}
	}

	ml.Info("daemon started",
		"pid", os.Getpid(),
		"socket", d.cfg.SocketAddr,
	)

	// 6. Wait for shutdown signal (explicit only — no idle timeout).
	select {
	case <-ctx.Done():
		ml.Info("shutdown requested")
	case <-d.shutdownCh:
		ml.Info("shutdown requested via IPC")
	}

	ml.Info("graceful shutdown sequence begin")
	// Graceful shutdown: HTTP → IPC → Modules
	if hs := d.httpServer.Load(); hs != nil {
		ml.Debug("stopping HTTP server")
		if err := hs.Stop(); err != nil {
			ml.Warn("HTTP server stop error", "error", err)
		}
	}

	ml.Debug("stopping IPC server")
	serverCancel()
	<-serverDone

	// Stop modules in reverse order.
	for i := len(d.modules) - 1; i >= 0; i-- {
		ml.Debug("stopping module", "module", d.modules[i].Name())
		if err := d.modules[i].OnStop(); err != nil {
			ml.Error("module stop failed", "module", d.modules[i].Name(), "err", err)
		}
	}

	ml.Info("daemon stopped", "pid", os.Getpid())
	d.store.Close()
	return nil
}

func (d *Daemon) writePID() error {
	return os.WriteFile(d.cfg.PIDFilePath, []byte(strconv.Itoa(os.Getpid())), 0o600)
}

func (d *Daemon) removePID() {
	os.Remove(d.cfg.PIDFilePath)
}
