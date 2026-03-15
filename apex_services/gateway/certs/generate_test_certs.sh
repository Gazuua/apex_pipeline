#!/bin/bash
# Test self-signed certificate generation
# DO NOT USE IN PRODUCTION!

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
    -days 365 -nodes \
    -subj "/C=KR/ST=Seoul/L=Seoul/O=ApexDev/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

echo "Generated: server.crt, server.key"
