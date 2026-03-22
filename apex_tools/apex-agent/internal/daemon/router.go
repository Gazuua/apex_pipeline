// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
)

type moduleRoutes struct {
	handlers map[string]HandlerFunc
}

type Router struct {
	mu      sync.RWMutex
	modules map[string]*moduleRoutes
}

func NewRouter() *Router {
	return &Router{modules: make(map[string]*moduleRoutes)}
}

func (r *Router) RegisterModule(name string, setup func(RouteRegistrar)) {
	mr := &moduleRoutes{handlers: make(map[string]HandlerFunc)}
	setup(mr)
	r.mu.Lock()
	r.modules[name] = mr
	r.mu.Unlock()
}

// Dispatch routes a request to the appropriate module handler.
// Returns (result, nil) on success, (nil, error) on failure.
func (r *Router) Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error) {
	r.mu.RLock()
	mr, ok := r.modules[module]
	r.mu.RUnlock()

	if !ok {
		return nil, fmt.Errorf("unknown module: %s", module)
	}

	handler, ok := mr.handlers[action]
	if !ok {
		return nil, fmt.Errorf("unknown action: %s.%s", module, action)
	}

	return handler(ctx, params, workspace)
}

func (mr *moduleRoutes) Handle(action string, handler HandlerFunc) {
	mr.handlers[action] = handler
}
