#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib.sh
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster

# Timing values govern how long voter promises remain valid and therefore must
# agree even when the resource definitions themselves are identical.
sed -i "s/lease_ms = $LEASE_MS/lease_ms = $((LEASE_MS + 100))/" \
    "$(node_config node2)"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3

wait_until 8 "clear quorum timing configuration mismatch error" \
    grep -Fq "configuration mismatch: quorum settings differ; check lease_ms/renew_ms/peer_timeout_ms" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log"

# The compatible quorum-only voter can still form quorum with node1.
wait_for_quorum node1

log "timing configuration mismatch handshake regression passed"
