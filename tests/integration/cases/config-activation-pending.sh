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
"$TEST_TMP/fixture" "$(node_config node1)" activation-reserved
"$TEST_TMP/fixture" "$(node_config node2)" activation-promised
"$TEST_TMP/fixture" "$(node_config node3)" activation-accepted
for node in node1 node2 node3; do
    for record in activation agreement application requests barrier; do
        cp "$(node_config "$node").state/$record.json" "$TEST_TMP/$node-$record.json"
    done
    sed -i 's@127.0.0.200/32@127.0.0.201/32@g' "$(node_config "$node")"
    start_node "$node"
done
pending_status() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["authority"]=="ini" and s["application_state"]=="frozen" and not s["admitted"] and not s["dynamic_commands"]
assert "readiness persisted" in s["error"]
assert all(n["connected"] and n["metadata_known"] and n["protocol"]==6 for n in s["nodes"])
' 2>/dev/null
}
for node in node1 node2 node3; do wait_until 10 "$node pending activation recovery" pending_status "$node"; done
sleep 2
for node in node1 node2 node3; do
    pending_status "$node" || die 'pending activation coordination lost liveness'
    "$LCS" -s "$(node_socket "$node")" --json status | python3 -c '
import json,sys
s=json.load(sys.stdin); assert not s["cluster"]["quorum"]
assert s["resources"][0]["address"]=="127.0.0.200/32" and s["resources"][0]["state"]!="active"
'
    "$LCS" -s "$(node_socket "$node")" --json config request-status interrupted-activation | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["state"]=="reserved" and s["history"]=="available" and s["value_revision"]==1
'
    if "$LCS" -s "$(node_socket "$node")" reload >"$TEST_TMP/$node-reload.log" 2>&1; then die 'pending activation accepted reload'; fi
    stop_node "$node"
    start_node "$node"
    index=$((${node#node}-1)); LCS_PIDS[$index]="${LCS_PIDS[-1]}"
    wait_until 10 "$node pending activation restart" pending_status "$node"
    for record in activation agreement application requests barrier; do
        cmp "$(node_config "$node").state/$record.json" "$TEST_TMP/$node-$record.json" || die 'pending activation rewrote durable evidence'
    done
done
# Required history and barrier proof must not be reconstructed after loss.
for node in node1 node2 node3; do
    stop_node "$node"
    record=requests; [[ "$node" != node2 ]] || record=barrier
    if [[ "$node" == node3 ]]; then
        # Valid checksum and empty history must not hide a lost accepted identity.
        python3 - "$(node_config "$node").state/requests.json" <<'PYTHON'
import hashlib,json,sys
path=sys.argv[1]; s=json.load(open(path)); s.pop('checksum'); s['requests']=[]
s['checksum']=hashlib.sha256(json.dumps(s,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()).hexdigest()
with open(path,'w') as f: json.dump(s,f,sort_keys=True,indent=2)
PYTHON
        cp "$(node_config "$node").state/requests.json" "$TEST_TMP/empty-requests.json"
    else
        rm "$(node_config "$node").state/$record.json"
    fi
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config "$node")" --no-syslog --no-timestamp >"$TEST_TMP/$node-missing.log" 2>&1; then die 'missing activation evidence accepted'; fi
    if [[ "$node" == node3 ]]; then
        cmp "$(node_config "$node").state/requests.json" "$TEST_TMP/empty-requests.json" || die 'lost identity was silently reconstructed'
    else
        [[ ! -e "$(node_config "$node").state/$record.json" ]] || die 'missing evidence recreated'
    fi
    cmp "$(node_config "$node").state/agreement.json" "$TEST_TMP/$node-agreement.json" || die 'missing evidence reset promises'
done
assert_no_vip_ops_in_log node3
log 'undecided activation reservations, promises and acceptances survive fenced restart unchanged'
