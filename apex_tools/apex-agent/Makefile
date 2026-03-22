.PHONY: build test lint clean

BINARY := apex-agent
GOFLAGS := -trimpath -ldflags="-s -w"

build:
	go build $(GOFLAGS) -o $(BINARY)$(if $(filter windows,$(GOOS)),.exe) ./cmd/apex-agent

test:
	go test ./... -race -cover -v

lint:
	go vet ./...

clean:
	rm -f $(BINARY) $(BINARY).exe

# Cross-compile for CI release
release-windows:
	GOOS=windows GOARCH=amd64 go build $(GOFLAGS) -o $(BINARY).exe ./cmd/apex-agent

release-linux:
	GOOS=linux GOARCH=amd64 go build $(GOFLAGS) -o $(BINARY) ./cmd/apex-agent
