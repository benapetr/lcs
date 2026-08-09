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

for node in node1 node2 node3; do
    config="$(node_config "$node")"
    printf '\n[vip vip2]\naddress = 127.0.0.201/32\ninterface = lo\n' >>"$config"
done

sed -i 's|address = 127.0.0.201/32|address = 127.0.0.202/32|' \
    "$(node_config node2)"
sed -i '/^\[vip vip1\]$/,/^interface = lo$/d' "$(node_config node1)"

log "verifying a mismatched candidate cannot commit"
kill -HUP "${LCS_PIDS[0]}"
sleep 2
if status_text node1 | grep -Fq "vip2 "; then
    die "mismatched resource candidate was committed"
fi
wait_for_owner node1 node1
wait_until 5 "clear candidate mismatch diagnostic" \
    grep -Fq "configuration reload candidate mismatch" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log" "$TEST_TMP/logs/node3.log"
if grep -Fq "configuration reload committed generation=2" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log" "$TEST_TMP/logs/node3.log"; then
    die "mismatched resource candidate reached commit"
fi
sed -i 's|address = 127.0.0.202/32|address = 127.0.0.201/32|' \
    "$(node_config node2)"
printf '\n[vip vip1]\naddress = 127.0.0.200/32\ninterface = lo\n' >>"$(node_config node1)"

log "reloading an added resource through lcs reload"
"$LCS" -s "$(node_socket node2)" reload |
    grep -Fq "configuration reload requested" ||
    die "lcs reload did not accept the reload request"
kill -HUP "${LCS_PIDS[0]}"
wait_until 15 "vip2 visible after coordinated reload" \
    node_status_has node1 "vip2 127.0.0.201/32 dev=lo state=active"
wait_for_owner node1 node1
for node in node1 node2 node3; do
    wait_until 10 "$node committed added resource" \
        grep -Fq "configuration reload committed generation=2 resources=2" "$TEST_TMP/logs/$node.log"
done

for node in node1 node2 node3; do
    config="$(node_config "$node")"
    sed -i '/^\[vip vip1\]$/,/^interface = lo$/d' "$config"
done

log "reloading removal of an active resource"
kill -HUP "${LCS_PIDS[1]}"
wait_until 15 "vip1 absent after coordinated reload" \
    bash -c "! '$LCS' -s '$(node_socket node1)' status 2>/dev/null | grep -Fq 'vip1 '"
wait_until 15 "vip2 remains active after removal" \
    node_status_has node1 "vip2 127.0.0.201/32 dev=lo state=active"
for node in node1 node2 node3; do
    wait_until 10 "$node committed removed resource" \
        grep -Fq "configuration reload committed generation=3 resources=1" "$TEST_TMP/logs/$node.log"
done

for node in node1 node2 node3; do
    sed -i 's|address = 127.0.0.201/32|address = 127.0.0.202/32|' \
        "$(node_config "$node")"
done
log "reloading a backend identity change"
kill -HUP "${LCS_PIDS[2]}"
wait_until 15 "changed VIP active after coordinated drain" \
    node_status_has node1 "vip2 127.0.0.202/32 dev=lo state=active"
for node in node1 node2 node3; do
    wait_until 10 "$node committed changed resource" \
        grep -Fq "configuration reload committed generation=4 resources=1" "$TEST_TMP/logs/$node.log"
done
sleep 1
if grep -Fq "configuration reload committed generation=5" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log" "$TEST_TMP/logs/node3.log"; then
    die "reload convergence triggered a duplicate no-op generation"
fi

log "verifying an unchanged lcs reload is a no-op"
"$LCS" -s "$(node_socket node1)" reload >/dev/null
wait_until 5 "unchanged reload diagnostic" \
    grep -Fq "configuration reload ignored: resource configuration is unchanged" \
    "$TEST_TMP/logs/node1.log"
if grep -Fq "configuration reload committed generation=5" \
    "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log" "$TEST_TMP/logs/node3.log"; then
    die "unchanged lcs reload committed another generation"
fi

sed -i '/^\[node node3\]$/,/^$/ s/role = quorum-only/role = full-member/' \
    "$(node_config node1)"
log "verifying live node membership changes are rejected"
kill -HUP "${LCS_PIDS[0]}"
wait_until 5 "membership reload rejection" \
    grep -Fq "node membership, roles, addresses, and ports cannot be reloaded" \
    "$TEST_TMP/logs/node1.log"
node_status_has node1 "vip2 127.0.0.202/32 dev=lo state=active" ||
    die "membership reload rejection disturbed the active resource"

log "resource reload regression passed"
