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
for node in node1 node2 node3; do
    config="$(node_config "$node")"
    state="$config.state"
    "$TEST_TMP/fixture" "$config" freeze
    cp "$state/activation.json" "$TEST_TMP/$node-frozen.json"
    python3 - "$state/activation.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1])); assert s['record_version']==3 and s['phase']=='frozen'
PY
    # New INI backends must not replace the durable baseline on restart.
    sed -i 's@127.0.0.200/32@127.0.0.201/32@g' "$config"
done
for node in node1 node2 node3; do start_node "$node"; done
frozen_status() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="ini" and s["application_state"]=="frozen" and not s["admitted"] and not s["dynamic_commands"] and not s["prepared"]
assert all(n["connected"] and n["metadata_known"] and n["protocol"]==6 for n in s["nodes"])
' 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 10 "$node frozen coordination startup" frozen_status "$node"
    "$LCS" -s "$(node_socket "$node")" --json status | python3 -c '
import json,sys
s=json.load(sys.stdin); assert not s["cluster"]["quorum"]
r=s["resources"]; assert len(r)==1 and r[0]["address"]=="127.0.0.200/32" and r[0]["state"]!="active"
'
    if "$LCS" -s "$(node_socket "$node")" reload >"$TEST_TMP/$node-reload.log" 2>&1; then die 'frozen node accepted INI reload'; fi
    if "$LCS" -s "$(node_socket "$node")" --json --request-id frozen-edit --expected-revision 0 resource update vip1 priority=19 >"$TEST_TMP/$node-edit.json"; then die 'frozen node accepted a definition edit'; fi
    "$LCS" -s "$(node_socket "$node")" config export >"$TEST_TMP/$node-export.json"
    python3 - "$TEST_TMP/$node-export.json" "$TEST_TMP/$node-frozen.json" <<'PY'
import json,sys
export=json.load(open(sys.argv[1])); frozen=json.loads(json.load(open(sys.argv[2]))['baseline'])
assert export==frozen
PY
    cmp "$(node_config "$node").state/activation.json" "$TEST_TMP/$node-frozen.json" || die 'startup changed freeze'
done
# Exercise multiple peer timeouts: metadata keeps the coordination channel alive
# while resource heartbeats, votes and placement remain fenced.
sleep 2
for node in node1 node2 node3; do frozen_status "$node" || die 'frozen coordination channel lost liveness'; done
# Signals also retain the freeze; no journal appears when every binary is online.
kill -HUP "${LCS_PIDS[0]}"
stop_node node3
start_node node3
LCS_PIDS[2]="${LCS_PIDS[3]}"
wait_until 10 'quorum-only frozen restart' frozen_status node3
for node in node1 node2 node3; do
    config="$(node_config "$node")"; state="$config.state"
    stop_node "$node"
    cmp "$state/activation.json" "$TEST_TMP/$node-frozen.json" || die 'restart or signal promoted freeze'
    for record in agreement application requests barrier; do [[ ! -e "$state/$record.json" ]] || die 'freeze alone created history'; done
    printf '{}\n' >"$state/agreement.json"
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$config" --no-syslog --no-timestamp >"$TEST_TMP/$node-conflict.log" 2>&1; then die 'frozen runtime accepted contradictory history'; fi
    cmp "$state/activation.json" "$TEST_TMP/$node-frozen.json" || die 'conflicting history replaced freeze'
    [[ "$(cat "$state/agreement.json")" == '{}' ]] || die 'conflicting history was overwritten'
    grep -Fq 'existing agreement/application/request history' "$TEST_TMP/$node-conflict.log" || die 'missing freeze/history conflict diagnostic'
done
assert_no_vip_ops_in_log node3
log 'durable freeze survives restart without promotion or fallback, including quorum-only members'
