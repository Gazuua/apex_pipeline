.PHONY: build test lint clean install release-windows release-linux

BINARY := apex-agent
VERSION = $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")

ifeq ($(OS),Windows_NT)
    INSTALL_DIR ?= $(LOCALAPPDATA)/apex-agent
    EXE = .exe
else
    INSTALL_DIR ?= $(HOME)/.local/bin
    EXE =
endif

GOFLAGS := -trimpath -ldflags="-s -w -X main.Version=$(VERSION)"

build:
	go build $(GOFLAGS) -o $(BINARY)$(if $(filter windows,$(GOOS)),.exe) ./cmd/apex-agent

test:
	go test ./... -race -cover -v

lint:
	go vet ./...

clean:
	rm -f $(BINARY) $(BINARY).exe

install:
	@mkdir -p $(INSTALL_DIR)
	go build -ldflags="-X main.Version=$(VERSION)" $(GOFLAGS) -o $(INSTALL_DIR)/apex-agent$(EXE) ./cmd/apex-agent
	@echo "installed: $(INSTALL_DIR)/apex-agent$(EXE) ($(VERSION))"

# Cross-compile for CI release
release-windows:
	GOOS=windows GOARCH=amd64 go build $(GOFLAGS) -o $(BINARY).exe ./cmd/apex-agent

release-linux:
	GOOS=linux GOARCH=amd64 go build $(GOFLAGS) -o $(BINARY) ./cmd/apex-agent
