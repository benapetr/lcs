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
wait_for_quorum node2
wait_for_quorum node3

for node in node1 node2; do
    printf '\n[vip vip2]\naddress = 127.0.0.201/32\ninterface = lo\n' \
        >>"$(node_config "$node")"
done
printf '\n[vip vip3]\naddress = 127.0.0.202/32\ninterface = lo\n' \
    >>"$(node_config node3)"

log "holding reload at candidate mismatch before participant loss"
kill -HUP "${LCS_PIDS[0]}"
wait_until 8 "candidate mismatch" \
    grep -Fq "configuration reload candidate mismatch" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log" \
    "$TEST_TMP/logs/node3.log"

stop_node node3
for node in node1 node2; do
    wait_until 8 "$node aborted the changed participant set" \
        grep -Fq "fixed online participant set changed" \
        "$TEST_TMP/logs/$node.log"
done
if grep -Fq "configuration reload committed generation=2" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log"; then
    die "transaction committed after losing a captured participant"
fi

log "retrying with the remaining online quorum"
"$LCS" -s "$(node_socket node2)" reload >/dev/null
wait_until 15 "vip2 active after retry" \
    node_status_has node1 "vip2 127.0.0.201/32 dev=lo state=active"
for node in node1 node2; do
    wait_until 10 "$node committed retried transaction" \
        grep -Fq "configuration reload committed generation=2 resources=2" \
        "$TEST_TMP/logs/$node.log"
done

log "resource reload participant-loss regression passed"
