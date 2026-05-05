#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node3
wait_for_owner node3 node1

assert_no_vip_ops_in_log node3

log "quorum-only no VIP manipulation regression passed"
