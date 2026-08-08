#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

LEASE_MS=900
RENEW_MS=200
PEER_TIMEOUT_MS=500
VIP_OP_DELAY_MS=1400

prepare_cluster
export LCS_EXTRA_ENV_node1="LCS_VIP_OP_DELAY_MS=$VIP_OP_DELAY_MS"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1

wait_until 10 "delayed VIP add to start" \
    grep -Fq "asynchronous VIP operation resource=vip1 op=1" \
    "$TEST_TMP/logs/node1.log"
node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=starting owner=node1" ||
    die "VIP was not starting while its add worker was delayed"
timeout 1 "$LCS" -s "$(node_socket node1)" status >/dev/null ||
    die "node1 CLI blocked during delayed VIP add"
sleep 1
node_status_has node1 "quorum: yes" ||
    die "node1 lost quorum during delayed VIP add"
wait_for_owner node1 node1

log "stopping VIP through a delayed asynchronous delete"
"$LCS" -s "$(node_socket node1)" resource stop vip1 >/dev/null
wait_until 5 "delayed VIP delete to start" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=stopping owner=node1"
timeout 1 "$LCS" -s "$(node_socket node1)" status >/dev/null ||
    die "node1 CLI blocked during delayed VIP delete"
node_status_has node2 "node1 role=full-member state=online" ||
    die "peer marked node1 offline during delayed VIP operation"
wait_until 8 "VIP delete completion" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=stopped owner=-"

log "asynchronous VIP backend operation regression passed"
