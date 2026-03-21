#!/bin/bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
set -euo pipefail

# Validate password does not contain single quotes (SQL injection prevention)
AUTH_ROLE_PASSWORD="${POSTGRES_AUTH_ROLE_PASSWORD:-auth_secret_change_me}"
if [[ "$AUTH_ROLE_PASSWORD" == *"'"* ]]; then
    echo "ERROR: POSTGRES_AUTH_ROLE_PASSWORD must not contain single quotes" >&2
    exit 1
fi

# Create auth_role with password from environment variable
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<-EOSQL
    DO \$\$
    BEGIN
        IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'auth_role') THEN
            CREATE ROLE auth_role WITH LOGIN PASSWORD '${POSTGRES_AUTH_ROLE_PASSWORD:-auth_secret_change_me}';
        END IF;
    END
    \$\$;
EOSQL

# Run the rest of the schema migration
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" \
    -f /docker-entrypoint-initdb.d/001_create_schema_and_role.sql
