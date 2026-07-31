#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib.sh
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster
fail_file="$TEST_TMP/fail-vip-del"
export LCS_EXTRA_ENV_node1="LCS_VIP_FAIL_DEL_FILE=$fail_file"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_owner node1 node1

log "forcing VIP stop failure on node1"
touch "$fail_file"
"$LCS" -s "$(node_socket node1)" resource stop vip1 >/dev/null
wait_until 8 "vip1 stop_failed on node1" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=stop_failed owner=node1"
node_status_has node1 "disabled=yes" || die "vip1 did not remain administratively stopped"
node_status_has node1 "stop_failed: local resource stop failed" || die "vip1 stop failure reason missing"

log "retrying VIP stop after failure is removed"
rm -f "$fail_file"
"$LCS" -s "$(node_socket node1)" resource stop vip1 >/dev/null
wait_until 8 "vip1 stopped after retry" \
    node_status_has node1 "vip1 127.0.0.200/32 dev=lo state=stopped owner=-"

log "stop_failed remediation regression passed"
