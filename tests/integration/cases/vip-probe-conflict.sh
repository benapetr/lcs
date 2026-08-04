#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster
export LCS_EXTRA_ENV_node1="LCS_VIP_CONFLICT=1"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1

wait_until 10 "asynchronous VIP conflict result" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=conflict owner=-"
wait_until 5 "VIP conflict propagation" \
    node_status_has node2 "vip1 127.0.0.200/32 dev=lo state=conflict owner=-"
if grep -Fq "VIP add 127.0.0.200/32" "$TEST_TMP/logs/node1.log"; then
    die "conflicting VIP was activated"
fi

log "asynchronous VIP conflict result regression passed"
