#!/bin/bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
set -euo pipefail

# md5 해시 생성: "md5" + md5(password + username)
PGB_PASS="${PGBOUNCER_PASSWORD:-apex}"
PGB_USER="${PGBOUNCER_USER:-apex}"
HASH=$(echo -n "${PGB_PASS}${PGB_USER}" | md5sum | cut -d' ' -f1)
echo "\"${PGB_USER}\" \"md5${HASH}\"" > /etc/pgbouncer/userlist.txt

exec pgbouncer /etc/pgbouncer/pgbouncer.ini
