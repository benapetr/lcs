#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_owner node1 node1

stop_node node2
stop_node node3
wait_for_node_offline node1 node2
wait_until 12 "node3 offline as seen by node1" \
    node_status_has node1 "node3 role=quorum-only state=offline"
wait_until 10 "node1 quorum loss" node_status_has node1 "quorum: no"
wait_until 10 "vip1 stopped after quorum loss" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=stopped owner=-"

start_node node2
start_node node3
wait_for_quorum node1
wait_for_owner node1 node1

log "quorum loss and recovery regression passed"
