#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077
prepare_cluster
read -r -a fixture_flags <<<"$(pkg-config --cflags --libs jansson libcrypto)"
"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 -Wall -Wextra -Wpedantic \
    "$SCRIPT_DIR/../helpers/config_recovery_fixture.c" \
    "$ROOT_DIR/src/"{config_requests,config_activation,config_barrier,config_agreement,config_application,config_store,config_candidate,resource_config,config,util,source_allowlist}.c \
    "${fixture_flags[@]}" -o "$TEST_TMP/fixture"
"$TEST_TMP/fixture" "$(node_config node1)" prepared
"$TEST_TMP/fixture" "$(node_config node2)" freeze
"$TEST_TMP/fixture" "$(node_config node3)" initializing
for node in node1 node2 node3; do
    sed -i 's@127.0.0.200/32@127.0.0.201/32@g' "$(node_config "$node")"
    start_node "$node"
done
coordination_ready() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["application_state"]=="frozen" and s["authority"]=="ini" and not s["admitted"] and not s["dynamic_commands"]
assert all(n["connected"] and n["metadata_known"] and n["protocol"]==6 for n in s["nodes"])
assert "all-member readiness persisted" in s["error"]
' "$1" 2>/dev/null
}
for node in node1 node2 node3; do wait_until 10 "$node partially initialized coordination" coordination_ready "$node"; done
sleep 2
for node in node1 node2 node3; do
    coordination_ready "$node" || die 'partially initialized peers lost coordination liveness'
    "$LCS" -s "$(node_socket "$node")" --json status | python3 -c '
import json,sys
s=json.load(sys.stdin); assert not s["cluster"]["quorum"]
assert s["resources"][0]["address"]=="127.0.0.200/32" and s["resources"][0]["state"]!="active"
'
    "$LCS" -s "$(node_socket "$node")" config export >"$TEST_TMP/$node-export.json"
    python3 - "$(node_config "$node").state" "$node" "$TEST_TMP/$node-export.json" <<'PY'
import json,sys
from pathlib import Path
state=Path(sys.argv[1]); intent=json.loads((state/'activation.json').read_text())
assert json.load(open(sys.argv[3])) == json.loads(intent['baseline'])
barrier=json.loads((state/'barrier.json').read_text())
assert barrier['ready_voters']==7 and barrier['membership']==intent['membership']
assert intent['record_version']==2 and intent['phase']=='prepared'
assert json.loads((state/'requests.json').read_text())['requests']==[]
assert all((state/(name+'.json')).exists() for name in ('agreement','application'))
PY
done
# Revision-zero preparation survives restart without rewriting retained journals.
for node in node1 node3; do
    for file in activation agreement application requests barrier; do cp "$(node_config "$node").state/$file.json" "$TEST_TMP/$node-$file.json"; done
    stop_node "$node"
    start_node "$node"
    index=$((${node#node}-1)); LCS_PIDS[$index]="${LCS_PIDS[-1]}"
    wait_until 10 "$node prepared restart" coordination_ready "$node"
    for file in activation agreement application requests barrier; do cmp "$(node_config "$node").state/$file.json" "$TEST_TMP/$node-$file.json" || die 'prepared restart rewrote history'; done
done
stop_node node1
rm "$(node_config node1).state/requests.json"
if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config node1)" --no-syslog --no-timestamp >"$TEST_TMP/missing-history.log" 2>&1; then die 'prepared restart accepted missing history'; fi
[[ ! -e "$(node_config node1).state/requests.json" ]] || die 'prepared restart recreated missing history'
cmp "$(node_config node1).state/activation.json" "$TEST_TMP/node1-activation.json" || die 'missing history changed intent'
grep -Fq 'request history' "$TEST_TMP/missing-history.log" || die 'missing-history diagnostic absent'
assert_no_vip_ops_in_log node3
log 'mixed frozen/prepared/interrupted initialization resumes coordination with unchanged history and resource fences'
