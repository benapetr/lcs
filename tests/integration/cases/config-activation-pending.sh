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
"$TEST_TMP/fixture" "$(node_config node2)" activation-accepted
"$TEST_TMP/fixture" "$(node_config node3)" activation-accepted
for node in node1 node2 node3; do
    for record in activation agreement application requests barrier; do
        cp "$(node_config "$node").state/$record.json" "$TEST_TMP/$node-$record.json"
    done
    sed -i 's@127.0.0.200/32@127.0.0.201/32@g' "$(node_config "$node")"
done
pending_status() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["authority"]=="ini" and s["application_state"]=="frozen" and not s["admitted"] and not s["dynamic_commands"]
assert "readiness persisted" in s["error"]
' 2>/dev/null
}
for node in node1 node2 node3; do
    start_node "$node"
    index=$((${node#node}-1)); LCS_PIDS[$index]="${LCS_PIDS[-1]}"
    wait_until 10 "$node isolated activation recovery" pending_status "$node"
    "$LCS" -s "$(node_socket "$node")" --json config request-status interrupted-activation | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"]=="reserved"'
    stop_node "$node"
    for record in activation agreement application requests barrier; do
        cmp "$(node_config "$node").state/$record.json" "$TEST_TMP/$node-$record.json" || die 'isolated activation rewrote durable evidence'
    done
    if [[ "$node" == node3 ]]; then
        python3 - "$(node_config "$node").state/requests.json" <<'PYTHON'
import hashlib,json,sys
path=sys.argv[1]; s=json.load(open(path)); s.pop('checksum'); s['requests']=[]
s['checksum']=hashlib.sha256(json.dumps(s,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()).hexdigest()
with open(path,'w') as f: json.dump(s,f,sort_keys=True,indent=2)
PYTHON
        cp "$(node_config "$node").state/requests.json" "$TEST_TMP/empty-requests.json"
        rm -f "$(node_socket "$node")"
        if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config "$node")" --no-syslog --no-timestamp >"$TEST_TMP/lost-identity.log" 2>&1; then die 'accepted activation lost its request identity'; fi
        [[ ! -S "$(node_socket "$node")" ]] || die 'lost identity opened listeners'
        cmp "$(node_config "$node").state/requests.json" "$TEST_TMP/empty-requests.json" || die 'lost identity was silently reconstructed'
        cmp "$(node_config "$node").state/agreement.json" "$TEST_TMP/$node-agreement.json" || die 'lost identity reset acceptance'
        cp "$TEST_TMP/$node-requests.json" "$(node_config "$node").state/requests.json"
    fi
done
for node in node1 node2 node3; do
    start_node "$node"
    index=$((${node#node}-1)); LCS_PIDS[$index]="${LCS_PIDS[-1]}"
done
admitted() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="json" and s["admitted"] and s["committed_revision"]==s["applied_revision"]==1
' 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 60 "$node automatic activation recovery" admitted "$node"
    "$LCS" -s "$(node_socket "$node")" --json config request-status interrupted-activation | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"]=="applied"'
done
for node in node1 node2 node3; do stop_node "$node"; done
# Required history and barrier proof must not be reconstructed after loss.
for node in node1 node2 node3; do
    stop_node "$node"
    record=requests; [[ "$node" != node2 ]] || record=barrier
    rm "$(node_config "$node").state/$record.json"
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config "$node")" --no-syslog --no-timestamp >"$TEST_TMP/$node-missing.log" 2>&1; then die 'missing activation evidence accepted'; fi
    [[ ! -e "$(node_config "$node").state/$record.json" ]] || die 'missing evidence recreated'
    cmp "$(node_config "$node").state/activation.json" "$TEST_TMP/$node-activation.json" || die 'missing evidence reset initialization'
done
assert_no_vip_ops_in_log node3
log 'isolated activation evidence is retained and automatic proposal recovery reaches admission'
