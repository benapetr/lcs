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
"$TEST_TMP/fixture" "$(node_config node1)" activation-chosen
"$TEST_TMP/fixture" "$(node_config node2)" activation-applying
"$TEST_TMP/fixture" "$(node_config node3)" activation-chosen
# Loss of required history must fail before listeners without resetting decision.
state="$(node_config node1).state"
cp "$state/requests.json" "$TEST_TMP/node1-requests.json"
rm "$state/requests.json"
if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config node1)" --no-syslog --no-timestamp >"$TEST_TMP/missing-history.log" 2>&1; then die 'chosen activation accepted lost history'; fi
[[ ! -S "$(node_socket node1)" && ! -e "$state/requests.json" ]] || die 'lost history opened listeners or recreated records'
cp "$TEST_TMP/node1-requests.json" "$state/requests.json"
touch "$TEST_TMP/cleanup-failure"
for node in node1 node2 node3; do
    cp "$(node_config "$node").state/agreement.json" "$TEST_TMP/$node-agreement.json"
    sed -i 's@127.0.0.200/32@127.0.0.201/32@g' "$(node_config "$node")"
    if [[ "$node" == node1 ]]; then
        LCS_VIP_FAIL_DEL_FILE="$TEST_TMP/cleanup-failure" start_node "$node"
    else
        start_node "$node"
    fi
done
blocked() {
    "$LCS" -s "$(node_socket node1)" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["application_state"]=="blocked" and not s["admitted"] and not s["dynamic_commands"]
assert s["committed_revision"]==1 and s["applied_revision"]==0 and s["pending_revision"]==1
' 2>/dev/null
}
wait_until 15 'activation blocked on failed baseline cleanup' blocked
cmp "$(node_config node1).state/agreement.json" "$TEST_TMP/node1-agreement.json" || die 'failed cleanup reset decision'
# Crash after durable BEGIN and failed physical cleanup. Restart must retry from
# the retained baseline and pending activation, with no client retry required.
kill -KILL "${LCS_PIDS[0]}"
wait "${LCS_PIDS[0]}" 2>/dev/null || true
rm "$TEST_TMP/cleanup-failure"
start_node node1
LCS_PIDS[0]="${LCS_PIDS[-1]}"
admitted() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="json" and s["admitted"] and s["committed_revision"]==s["applied_revision"]==1
' 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 40 "$node chosen activation admission" admitted "$node"
    wait_for_quorum "$node"
    "$LCS" -s "$(node_socket "$node")" --json config request-status interrupted-activation | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"]=="applied"'
    "$LCS" -s "$(node_socket "$node")" config export >"$TEST_TMP/$node-export.json"
    python3 - "$TEST_TMP/$node-export.json" "$(node_config "$node").state/activation.json" <<'PYTHON'
import json,sys
current=json.load(open(sys.argv[1])); baseline=json.loads(json.load(open(sys.argv[2]))['baseline'])
assert current['schema_version']==2 and current['revision']==1
for resource in current['resources'].values():
    assert resource.pop('birth_revision')==0
assert current['resources']==baseline['resources'] and current['groups']==baseline['groups']
PYTHON
    cmp "$(node_config "$node").state/agreement.json" "$TEST_TMP/$node-agreement.json" || die 'initial application changed chosen decision'
done
assert_no_vip_ops_in_log node3
log 'chosen activation applies from baseline or pending catalog through fenced recovery'
