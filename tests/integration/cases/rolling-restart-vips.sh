#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster
for node in node1 node2 node3; do
    cat >>"$(node_config "$node")" <<'EOF'
group = vips
priority = 2

[group vips]
type = keep-together
mode = best-effort

[vip vip1_v6]
address = 2001:db8::200/128
interface = lo
group = vips
priority = 1
EOF
done

wait_for_both_vips()
{
    local observer="$1" owner="$2"
    wait_for_owner "$observer" "$owner"
    wait_until 15 "IPv6 VIP owner $owner as seen by $observer" \
        node_status_has "$observer" "vip1_v6 2001:db8::200/128 dev=lo state=active owner=$owner"
}

assert_agreement()
{
    python3 - "$LCS" "$(node_socket node1)" "$(node_socket node2)" "$(node_socket node3)" <<'PY'
import json
import subprocess
import sys

views = [json.loads(subprocess.check_output([sys.argv[1], "-s", sock, "--json", "status"]))
         for sock in sys.argv[2:]]
expected = None
for view in views:
    assert view["cluster"]["quorum"]
    assert all(node["state"] == "online" for node in view["nodes"])
    resources = [(res["name"], res["state"], res["owner"], res["epoch"], res["lease_id"])
                 for res in view["resources"]]
    assert all(res[1] == "active" and res[2] == "node1" for res in resources)
    if expected is None:
        expected = resources
    assert resources == expected, (resources, expected)
PY
}

start_node node1
start_node node2
start_node node3
for node in node1 node2 node3; do
    wait_for_socket "$node"
    wait_for_quorum "$node"
    wait_for_both_vips "$node" node1
done

"$LCS" -s "$(node_socket node1)" resource move vip1 node2
for node in node1 node2 node3; do
    wait_for_both_vips "$node" node2
done
stop_node node1
cp "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node1-before-restart.log"
start_node node1
wait_for_quorum node1
wait_until 12 "node1 restart recovery complete" \
    node_status_has node1 "node1 role=full-member state=online (self)"
wait_for_both_vips node1 node2

"$LCS" -s "$(node_socket node2)" resource move vip1 node1
for node in node1 node2 node3; do
    wait_for_both_vips "$node" node1
done
stop_node node2
cp "$TEST_TMP/logs/node2.log" "$TEST_TMP/logs/node2-before-restart.log"
start_node node2
wait_for_quorum node2
wait_until 12 "node2 restart recovery complete" \
    node_status_has node2 "node2 role=full-member state=online (self)"
for node in node1 node2 node3; do
    wait_for_both_vips "$node" node1
done

wait_until 12 "all nodes agree after rolling restart" assert_agreement
# Check continued agreement over multiple renewals, rather than only the first
# successful reconnect after recovery.
for ((round = 0; round < 8; round++)); do
    sleep 0.5
    assert_agreement || die "cluster diverged after rolling restart"
done
if grep -Eq 'protocol error|before HELLO' "$TEST_TMP"/logs/*.log; then
    die "peer protocol failure during rolling restart"
fi

log "grouped IPv4/IPv6 rolling restart regression passed"
