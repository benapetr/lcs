#!/usr/bin/env bash
set -euo pipefail

LEASE_MS=900
RENEW_MS=200
PEER_TIMEOUT_MS=600

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib.sh
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_owner node1 node1

# SIGKILL deliberately loses every daemon's lease and grant memory and skips
# graceful release. This is the diskless recovery case, not normal shutdown.
old_pids=("${LCS_PIDS[@]}")
for pid in "${old_pids[@]}"; do
    kill -9 "$pid" 2>/dev/null || true
done
for pid in "${old_pids[@]}"; do
    wait "$pid" 2>/dev/null || true
done

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3

node_status_has node1 "node1 role=full-member state=recovering (self)" ||
    die "node1 did not enter recovery after the full-cluster restart"
node_status_has node2 "node2 role=full-member state=recovering (self)" ||
    die "node2 did not enter recovery after the full-cluster restart"
node_status_has node3 "node3 role=quorum-only state=recovering (self)" ||
    die "node3 did not enter recovery after the full-cluster restart"
node_status_has node1 "quorum: no" ||
    die "restarted cluster formed quorum before recovery completed"

wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3
wait_for_owner node1 node1
wait_for_owner node2 node1

log "full in-memory cluster restart regression passed"
