#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GRANT_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-quorum-grants.XXXXXX")"
OPERATIONS_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-lease-operations.XXXXXX")"
RECOVERY_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-recovery.XXXXXX")"
SYNC_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-state-sync.XXXXXX")"
CONFIG_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-config-fingerprint.XXXXXX")"
trap 'rm -f "$GRANT_BIN" "$OPERATIONS_BIN" "$RECOVERY_BIN" "$SYNC_BIN" "$CONFIG_BIN"' EXIT

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/config_fingerprint.c" \
    "$ROOT_DIR/src/config.c" \
    -Wl,--gc-sections -o "$CONFIG_BIN"
"$CONFIG_BIN"
echo "configuration fingerprint unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/quorum_grants.c" \
    "$ROOT_DIR/src/lease.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$GRANT_BIN"
"$GRANT_BIN"
echo "quorum grant unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/lease_operations.c" \
    "$ROOT_DIR/src/lease.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$OPERATIONS_BIN"
"$OPERATIONS_BIN"
echo "lease operation unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/recovery.c" \
    "$ROOT_DIR/src/cluster.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$RECOVERY_BIN"
"$RECOVERY_BIN"
echo "restart recovery unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/state_sync.c" \
    "$ROOT_DIR/src/cluster.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$SYNC_BIN"
"$SYNC_BIN"
echo "state synchronization unit tests passed"
