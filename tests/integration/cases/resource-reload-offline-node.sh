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
wait_for_owner node1 node1

stop_node node1
wait_for_node_offline node2 node1

for node in node2 node3; do
    printf '\n[vip vip2]\naddress = 127.0.0.201/32\ninterface = lo\n' \
        >>"$(node_config "$node")"
done

log "reloading resources with node1 offline"
"$LCS" -s "$(node_socket node3)" reload |
    grep -Fq "configuration reload requested" ||
    die "online quorum rejected resource reload"
wait_until 15 "vip2 visible after online-quorum reload" \
    node_status_has node2 "vip2 127.0.0.201/32 dev=lo state=active owner=node2"
for node in node2 node3; do
    wait_until 10 "$node committed with two online participants" \
        grep -Fq "configuration reload committed generation=2 resources=2" \
        "$TEST_TMP/logs/$node.log"
    grep -Fq "participants=2/3" "$TEST_TMP/logs/$node.log" ||
        die "$node did not use the fixed two-node participant set"
done

log "verifying stale node1 is rejected with an actionable diagnostic"
start_node node1
wait_for_socket node1
wait_until 15 "stale configuration catch-up proof" \
    grep -Fq "committed configuration proof received" \
    "$TEST_TMP/logs/node1.log"
printf '\n[vip vip2]\naddress = 127.0.0.201/32\ninterface = lo\n' \
    >>"$(node_config node1)"

log "reloading the committed configuration on stale node1"
"$LCS" -s "$(node_socket node1)" reload |
    grep -Fq "configuration reload requested" ||
    die "stale node did not accept committed configuration catch-up"
wait_until 8 "stale-node catch-up applied" \
    grep -Fq "configuration reload entering stale-node catch-up" \
    "$TEST_TMP/logs/node1.log"
wait_for_quorum node1
wait_until 10 "node1 rejoined after config correction" \
    node_status_has node2 "node1 role=full-member state=online"

log "online-quorum resource reload regression passed"
