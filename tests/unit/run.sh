#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GRANT_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-quorum-grants.XXXXXX")"
SYNC_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-state-sync.XXXXXX")"
trap 'rm -f "$GRANT_BIN" "$SYNC_BIN"' EXIT

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/quorum_grants.c" \
    "$ROOT_DIR/src/lease.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$GRANT_BIN"
"$GRANT_BIN"
echo "quorum grant unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/state_sync.c" \
    "$ROOT_DIR/src/cluster.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$SYNC_BIN"
"$SYNC_BIN"
echo "state synchronization unit tests passed"
