#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

vip_home_blocked()
{
    status_text node1 2>/dev/null | grep -F "vip1 127.0.0.200/32 dev=lo state=active owner=node1" | grep -Fq "home=node2 blocked=yes"
}

write_home_config()
{
    local self="$1"
    write_config "$self"
    printf 'home_node = node2\n' >>"$(node_config "$self")"
}

trap cleanup_cluster EXIT

require_binaries
mkdir -p "$TEST_TMP/logs"
write_home_config node1
write_home_config node2
write_home_config node3

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3
wait_for_owner node1 node2

log "manual move away from home blocks automatic home rebalance"
"$LCS" -s "$(node_socket node1)" move vip1 node1
wait_for_owner node1 node1
wait_until 8 "vip1 home block visible" vip_home_blocked

sleep 2
wait_for_owner node1 node1

log "manual move back home clears automatic home rebalance block"
"$LCS" -s "$(node_socket node1)" move vip1 node2
wait_for_owner node1 node2
wait_for_owner node2 node2

log "home-node regression passed"
