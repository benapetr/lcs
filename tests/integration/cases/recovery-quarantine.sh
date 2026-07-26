#!/usr/bin/env bash
set -euo pipefail

LEASE_MS=800
RENEW_MS=200
PEER_TIMEOUT_MS=500

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster
start_node node1
wait_for_socket node1
node_status_has node1 "node1 role=full-member state=recovering (self)" ||
    die "local status did not expose restart recovery state"

# Time quarantine alone is insufficient: without an initial sync from enough
# peers, a restarted node must remain non-voting indefinitely.
sleep 1.6
node_status_has node1 "quorum: no (0 votes" ||
    die "isolated node became voting-ready without quorum state synchronization"
node_status_has node1 "node1 role=full-member state=recovering (self)" ||
    die "isolated node left recovery without synchronization"

start_node node2
wait_for_socket node2

# node1's timer has elapsed, but node2 must not contribute its vote until its
# own restart quarantine has elapsed. Allow the normal reconnect backoff before
# requiring node1 to observe node2's recovery state.
wait_until 2 "node2 visible as recovering" \
    node_status_has node1 "node2 role=full-member state=recovering"
node_status_has node1 "quorum: no" ||
    die "recovering node contributed a vote before its quarantine elapsed"

wait_for_quorum node1
wait_for_quorum node2
wait_for_owner node1 node1
node_status_has node1 "node2 role=full-member state=online" ||
    die "peer did not transition from recovering to online"

grep -Fq "recovery complete; node is now eligible to vote" "$TEST_TMP/logs/node1.log" ||
    die "node1 did not log recovery completion"
grep -Fq "recovery complete; node is now eligible to vote" "$TEST_TMP/logs/node2.log" ||
    die "node2 did not log recovery completion"

log "restart recovery quarantine regression passed"
