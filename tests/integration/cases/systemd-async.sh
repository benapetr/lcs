#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

# Each operation is deliberately slower than both peer failure detection and a
# complete lease.  The daemon must continue serving sockets and renewing while
# the worker waits for systemd.
LEASE_MS=900
RENEW_MS=200
PEER_TIMEOUT_MS=500
SYSTEMD_DELAY_MS=1600

prepare_cluster
for node in node1 node2 node3; do
    printf '\n[service app]\nsystemd_unit = app.service\n' >>"$(node_config "$node")"
done
export LCS_EXTRA_ENV_node1="LCS_SYSTEMD_DELAY_MS=$SYSTEMD_DELAY_MS"
export LCS_EXTRA_ENV_node2="LCS_SYSTEMD_DELAY_MS=$SYSTEMD_DELAY_MS"
export LCS_EXTRA_ENV_node3="LCS_SYSTEMD_DELAY_MS=$SYSTEMD_DELAY_MS"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1

wait_until 10 "delayed service activation to enter starting" \
    node_status_has node1 "app type=service unit=app.service state=starting owner=node1"

# A synchronous systemd call would leave this local CLI request unanswered.
timeout 1 "$LCS" -s "$(node_socket node1)" status >/dev/null ||
    die "node1 CLI blocked during delayed systemd start"
sleep 1
node_status_has node1 "quorum: yes" ||
    die "node1 lost quorum while its systemd start was pending"
node_status_has node1 "app type=service unit=app.service state=starting owner=node1" ||
    die "service lease did not remain in starting state during delayed start"
wait_until 8 "delayed service activation to complete" \
    node_status_has node1 "app type=service unit=app.service state=active owner=node1"
wait_until 5 "delayed service health worker" \
    grep -Fq "asynchronous systemd operation resource=app op=4" "$TEST_TMP/logs/node1.log"

log "moving service while a delayed health worker must be cancelled"
move_output="$TEST_TMP/move.out"
"$LCS" -s "$(node_socket node1)" resource move app node2 >"$move_output" 2>&1 &
move_pid=$!
wait_until 5 "service handoff to enter stopping" \
    node_status_has node1 "app type=service unit=app.service state=stopping owner=node1"
wait_until 3 "nonblocking health-worker cancellation" \
    grep -Fq "requested asynchronous systemd worker cancellation" "$TEST_TMP/logs/node1.log"
timeout 1 "$LCS" -s "$(node_socket node1)" status >/dev/null ||
    die "node1 CLI blocked during delayed systemd stop"
sleep 1
node_status_has node2 "node1 role=full-member state=online" ||
    die "peer marked node1 offline while its systemd stop was pending"

if ! wait "$move_pid"; then
    sed -n '1,80p' "$move_output" >&2 || true
    die "service move failed"
fi
wait_until 10 "service activation on handoff target" \
    node_status_has node2 "app type=service unit=app.service state=active owner=node2"

log "asynchronous systemd operation regression passed"
