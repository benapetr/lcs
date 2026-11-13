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
wait_for_socket node1
wait_for_quorum node1
wait_for_owner node1 node1
sleep 1
for node in node1 node2; do
    [[ ! -e "$(node_config "$node").state/activation.json" ]] || die 'offline witness did not block first freeze'
done
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
activated_status()
{
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["authority"]=="json" and s["admitted"]
assert s["dynamic_commands"] == (next(n for n in s["nodes"] if n["self"])["role"] != "quorum-only")
assert s["committed_revision"]==s["applied_revision"]==int(sys.argv[2])
assert sum(n["connected"] for n in s["nodes"])==int(sys.argv[1])
' "${2:-3}" "${3:-1}" 2>/dev/null
}
for node in node1 node2 node3; do
    cp "$(node_config "$node")" "$TEST_TMP/$node-human.conf"
    wait_until 45 "$node automatically activated matching INI definitions" activated_status "$node"
    cmp "$(node_config "$node")" "$TEST_TMP/$node-human.conf" || die 'migration rewrote human config'
    "$LCS" -s "$(node_socket "$node")" config export >"$TEST_TMP/$node-export.json"
    python3 - "$TEST_TMP/$node-export.json" <<'PYJSON'
import json,sys
s=json.load(open(sys.argv[1])); assert s['schema_version']==2 and s['revision']==1
assert s['resources']['vip1']['priority']==42 and s['resources']['vip1']['address']=='127.0.0.200/32'
PYJSON
done
wait_for_owner node1 node1
assert_no_vip_ops_in_log node3

log "checking offline members do not retain session metadata"
stop_node node3
wait_until 10 "offline member excluded from prepared matching set" activated_status node1 2
"$LCS" -s "$(node_socket node1)" --json config status | python3 -c '
import json, sys
node = next(n for n in json.load(sys.stdin)["nodes"] if n["name"] == "node3")
assert node["metadata_known"] is False and node["prepared"] is None
assert node["baseline_digest"] is None and node["protocol"] == 0
'
# Dynamic commands must work after the automatic upgrade, using the ordinary
# full-member majority while the witness is offline.
for attempt in {1..20}; do
    result=0
    "$LCS" -s "$(node_socket node1)" --json --request-id auto-upgrade-create \
        resource create vip2 vip address=127.0.0.204/32 interface=lo >"$TEST_TMP/create.json" || result=$?
    if (( result != 1 )); then break; fi
    grep -Eq 'busy|not admitted' "$TEST_TMP/create.json" || die 'automatic upgrade did not permit resource creation'
    sleep .2
done
(( result != 1 )) || die 'resource creation remained busy after automatic upgrade'
for node in node1 node2; do
    wait_until 45 "$node dynamic resource creation with witness offline" activated_status "$node" 2 2
done
"$LCS" -s "$(node_socket node1)" --json --request-id auto-upgrade-create \
    resource create vip2 vip address=127.0.0.204/32 interface=lo >"$TEST_TMP/retry-create.json"
python3 - "$TEST_TMP/retry-create.json" <<'PYJSON'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='applied' and s['committed_revision']==2
PYJSON
start_node node3
wait_for_socket node3
wait_for_quorum node3
for node in node1 node2 node3; do
    wait_until 45 "$node recovered automatic JSON admission after reconnect" activated_status "$node" 3 2
done
wait_for_owner node1 node1
log "automatic INI migration waits for offline/mismatching witnesses, retains human config and recovers JSON admission"
