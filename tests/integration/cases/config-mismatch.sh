#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster

# Per-node cluster settings already differ in the generated configs. A quorum-
# only node may also carry different execution details because it never starts
# resources; neither kind of difference should prevent it from voting.
sed -i 's|address = 127.0.0.200/32|address = 127.0.0.201/32|' "$(node_config node3)"

# Full members, however, must agree on the resource they may execute.
sed -i 's|address = 127.0.0.200/32|address = 127.0.0.202/32|' "$(node_config node2)"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3

wait_until 8 "clear full-member configuration mismatch error" \
    grep -Fq "configuration mismatch: full-member resources differ" "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log"

# node1 and the quorum-only node still have a compatible voting schema.
wait_for_quorum node1

log "configuration mismatch handshake regression passed"
