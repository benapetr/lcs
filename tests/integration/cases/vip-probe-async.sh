#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

LEASE_MS=900
RENEW_MS=200
PEER_TIMEOUT_MS=500
VIP_PROBE_DELAY_MS=1600

prepare_cluster
export LCS_EXTRA_ENV_node1="LCS_VIP_PROBE_DELAY_MS=$VIP_PROBE_DELAY_MS"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1

wait_until 10 "delayed VIP probe to enter starting" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=starting owner=node1"
timeout 1 "$LCS" -s "$(node_socket node1)" status >/dev/null ||
    die "node1 CLI blocked during delayed VIP probe"

log "cancelling delayed VIP probe before address activation"
"$LCS" -s "$(node_socket node1)" resource stop vip1
wait_until 5 "VIP probe cancellation" \
    grep -Fq "requested asynchronous VIP probe cancellation" "$TEST_TMP/logs/node1.log"
wait_until 5 "cancelled VIP to become stopped" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=stopped owner=-"
if grep -Fq "VIP add 127.0.0.200/32" "$TEST_TMP/logs/node1.log"; then
    die "cancelled VIP probe activated the address"
fi

log "starting VIP with a probe longer than its lease"
"$LCS" -s "$(node_socket node1)" resource start vip1
wait_until 8 "second delayed VIP probe" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=starting owner=node1"
sleep 1
node_status_has node1 "quorum: yes" ||
    die "node1 lost quorum while its VIP probe was pending"
node_status_has node2 "node1 role=full-member state=online" ||
    die "peer marked node1 offline while its VIP probe was pending"
node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=starting owner=node1" ||
    die "VIP activation lease was not renewed during delayed probe"
wait_for_owner node1 node1

log "asynchronous VIP conflict probe regression passed"
