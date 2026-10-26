#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GRANT_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-quorum-grants.XXXXXX")"
OPERATIONS_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-lease-operations.XXXXXX")"
RECOVERY_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-recovery.XXXXXX")"
SYNC_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-state-sync.XXXXXX")"
CONFIG_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-config-fingerprint.XXXXXX")"
PROTOCOL_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-protocol-negotiation.XXXXXX")"
PEER_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-peer-broadcasts.XXXXXX")"
ALLOWLIST_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-source-allowlist.XXXXXX")"
RESOURCE_CONFIG_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-resource-json.XXXXXX")"
STORE_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-config-store.XXXXXX")"
BOOTSTRAP_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-config-bootstrap.XXXXXX")"
AGREEMENT_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-config-agreement.XXXXXX")"
CANDIDATE_BIN="$(mktemp "${TMPDIR:-/tmp}/lcs-config-candidate.XXXXXX")"
trap 'rm -f "$GRANT_BIN" "$OPERATIONS_BIN" "$RECOVERY_BIN" "$SYNC_BIN" "$CONFIG_BIN" "$PROTOCOL_BIN" "$PEER_BIN" "$ALLOWLIST_BIN" "$RESOURCE_CONFIG_BIN" "$STORE_BIN" "$BOOTSTRAP_BIN" "$AGREEMENT_BIN" "$CANDIDATE_BIN"' EXIT

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/tests/unit/config_candidate.c" "$ROOT_DIR/src/config_candidate.c" \
    "$ROOT_DIR/src/config.c" "$ROOT_DIR/src/util.c" "$ROOT_DIR/src/source_allowlist.c" \
    -o "$CANDIDATE_BIN"
"$CANDIDATE_BIN"

read -ra JSON_CFLAGS <<< "$(pkg-config --cflags jansson)"
read -ra JSON_LIBS <<< "$(pkg-config --libs jansson)"
read -ra CRYPTO_CFLAGS <<< "$(pkg-config --cflags libcrypto)"
read -ra CRYPTO_LIBS <<< "$(pkg-config --libs libcrypto)"
"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic "${JSON_CFLAGS[@]}" "${CRYPTO_CFLAGS[@]}" \
    "$ROOT_DIR/tests/unit/config_agreement.c" "$ROOT_DIR/src/config_agreement.c" \
    "$ROOT_DIR/src/config_agreement_worker.c" \
    "$ROOT_DIR/src/config_transfer.c" "$ROOT_DIR/src/protocol.c" \
    "$ROOT_DIR/src/config_store.c" "$ROOT_DIR/src/config_candidate.c" "$ROOT_DIR/src/resource_config.c" \
    "$ROOT_DIR/src/config.c" "$ROOT_DIR/src/util.c" "$ROOT_DIR/src/source_allowlist.c" \
    -Wl,--wrap=fsync "${JSON_LIBS[@]}" "${CRYPTO_LIBS[@]}" -o "$AGREEMENT_BIN"
"$AGREEMENT_BIN"
"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic "${JSON_CFLAGS[@]}" \
    "$ROOT_DIR/tests/unit/resource_config.c" "$ROOT_DIR/src/resource_config.c" "$ROOT_DIR/src/config_candidate.c" \
    "$ROOT_DIR/src/config.c" "$ROOT_DIR/src/util.c" "$ROOT_DIR/src/source_allowlist.c" \
    "${JSON_LIBS[@]}" -o "$RESOURCE_CONFIG_BIN"
"$RESOURCE_CONFIG_BIN"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic \
    "$ROOT_DIR/tests/unit/config_store.c" "$ROOT_DIR/src/config_store.c" \
    -Wl,--wrap=fsync -Wl,--wrap=renameat -o "$STORE_BIN"
"$STORE_BIN"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic "${JSON_CFLAGS[@]}" \
    "$ROOT_DIR/tests/unit/config_bootstrap.c" "$ROOT_DIR/src/config_bootstrap.c" \
    "$ROOT_DIR/src/config_store.c" "$ROOT_DIR/src/config_candidate.c" "$ROOT_DIR/src/resource_config.c" \
    "$ROOT_DIR/src/config.c" "$ROOT_DIR/src/util.c" "$ROOT_DIR/src/source_allowlist.c" \
    "${JSON_LIBS[@]}" -o "$BOOTSTRAP_BIN"
"$BOOTSTRAP_BIN"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/source_allowlist.c" \
    "$ROOT_DIR/src/config.c" "$ROOT_DIR/src/util.c" "$ROOT_DIR/src/source_allowlist.c" \
    -Wl,--gc-sections -o "$ALLOWLIST_BIN"
"$ALLOWLIST_BIN"
echo "source IP allowlist unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/peer_broadcasts.c" \
    "$ROOT_DIR/src/peer.c" "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$PEER_BIN"
"$PEER_BIN"
echo "peer broadcast unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/protocol_negotiation.c" \
    "$ROOT_DIR/src/protocol.c" \
    -Wl,--gc-sections -o "$PROTOCOL_BIN"
"$PROTOCOL_BIN"
echo "protocol negotiation unit tests passed"

"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 \
    -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
    "$ROOT_DIR/tests/unit/config_fingerprint.c" \
    "$ROOT_DIR/src/config.c" "$ROOT_DIR/src/util.c" "$ROOT_DIR/src/source_allowlist.c" \
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
