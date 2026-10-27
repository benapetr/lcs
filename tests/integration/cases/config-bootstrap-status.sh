#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT

config_status_matches()
{
    local node="$1" matching="$2" connected="${3:-3}"
    "$LCS" -s "$(node_socket "$node")" --json config status 2>/dev/null | python3 -c '
import json, sys
value = json.load(sys.stdin)
assert value["authority"] == "ini"
assert value["dynamic_commands"] is False and value["activation_ready"] is False
assert value["configured_members"] == 3 and value["matching_members"] == int(sys.argv[1])
assert sum(n["connected"] for n in value["nodes"]) == int(sys.argv[2])
assert len(value["baseline_digest"]) == 64 and len(value["membership"]) == 64
assert next(n for n in value["nodes"] if n["name"] == "node3")["role"] == "quorum-only"
' "$matching" "$connected" 2>/dev/null
}

prepare_cluster
# Legacy voting fingerprints do not include hooks; full JSON digests must.
printf '\n' >>"$(node_config node3)"
sed -i '/^\[vip vip1\]$/a post_start = /bin/true' "$(node_config node3)"
start_node node1
start_node node2
start_node node3
for node in node1 node2 node3; do
    wait_for_socket "$node"
    wait_for_quorum "$node"
done
wait_for_owner node1 node1
log "checking full-definition mismatches are visible without breaking legacy peering"
wait_until 10 "node1 sees its own and node2 matching snapshots" config_status_matches node1 2
wait_until 10 "quorum-only member reports its different snapshot" config_status_matches node3 1
"$LCS" -s "$(node_socket node1)" config status | grep -Fq "Dynamic commands: unavailable" ||
    die "configuration status incorrectly enabled dynamic commands"

log "checking a coordinated INI reload refreshes all peer digests"
sed -i '/^post_start = \/bin\/true$/d' "$(node_config node3)"
# Give every legacy participant a real edit so unchanged nodes do not treat
# this transaction as a no-op while node3 updates its formerly local hook.
for node in node1 node2 node3; do
    sed -i '/^\[vip vip1\]$/a priority = 42' "$(node_config "$node")"
done
"$LCS" -s "$(node_socket node3)" reload >/dev/null
for node in node1 node2 node3; do
    wait_until 15 "$node has three matching prepared snapshots" config_status_matches "$node" 3
done
wait_for_owner node1 node1

log "checking offline members do not retain session metadata"
stop_node node3
wait_until 10 "offline member excluded from prepared matching set" config_status_matches node1 2 2
"$LCS" -s "$(node_socket node1)" --json config status | python3 -c '
import json, sys
node = next(n for n in json.load(sys.stdin)["nodes"] if n["name"] == "node3")
assert node["metadata_known"] is False and node["prepared"] is None
assert node["baseline_digest"] is None and node["protocol"] == 0
'
start_node node3
wait_for_socket node3
wait_for_quorum node3
for node in node1 node2 node3; do
    wait_until 10 "$node received fresh metadata after reconnect" config_status_matches "$node" 3
done
wait_for_owner node1 node1
log "configuration bootstrap metadata regression passed"
