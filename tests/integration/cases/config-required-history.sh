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
    "$TEST_TMP/fixture" "$(node_config "$node")" required-history
    "$TEST_TMP/fixture" "$(node_config "$node")" history
    cp "$(node_config "$node").state/requests.json" "$TEST_TMP/$node-requests.json"
    cp "$(node_config "$node").state/activation.json" "$TEST_TMP/$node-activation.json"
    python3 - "$(node_config "$node").state/activation.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1])); assert s['record_version']==2 and s['phase']=='prepared'
PY
done
for node in node1 node2; do
    state="$(node_config "$node").state"
    if [[ "$node" == node1 ]]; then rm "$state/requests.json"; else printf '{}\n' >"$state/requests.json"; fi
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config "$node")" --no-syslog --no-timestamp >"$TEST_TMP/$node-refused.log" 2>&1; then
        die 'prepared format-2 initialization accepted missing/corrupt request history'
    fi
    [[ ! -S "$(node_socket "$node")" ]] || die 'listener opened before mandatory history recovery'
    cmp "$state/activation.json" "$TEST_TMP/$node-activation.json" || die 'history failure changed initialization intent'
    grep -Fq 'request history' "$TEST_TMP/$node-refused.log" || die 'missing mandatory history recovery error'
    if [[ "$node" == node1 ]]; then
        [[ ! -e "$state/requests.json" ]] || die 'prepared initialization silently recreated history'
    else
        [[ "$(cat "$state/requests.json")" == '{}' ]] || die 'corrupt history was overwritten'
    fi
    cp "$TEST_TMP/$node-requests.json" "$state/requests.json"
done
start_node node1
start_node node2
start_node node3
admitted() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="json" and s["admitted"] and s["committed_revision"]==s["applied_revision"]==int(sys.argv[1])
' "${2:-3}" 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 40 "$node required-history admission" admitted "$node"
    wait_for_quorum "$node"
    "$LCS" -s "$(node_socket "$node")" --json config request-status recovery-3 | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"]=="applied"'
done
for attempt in {1..20}; do
    result=0
    "$LCS" -s "$(node_socket node1)" --json --request-id mandatory-history-edit --expected-revision 3 resource update vip1 priority=19 >"$TEST_TMP/edit.json" || result=$?
    (( result != 1 )) && break
    grep -Eq 'busy|not admitted' "$TEST_TMP/edit.json" || die 'mandatory-history mutation rejected'
    sleep .2
done
(( result != 1 )) || die 'mandatory-history mutation remained busy'
for node in node1 node2 node3; do wait_until 40 "$node mandatory-history edit" admitted "$node" 4; done
"$LCS" -s "$(node_socket node1)" --json config request-status mandatory-history-edit | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"]=="applied"'
assert_no_vip_ops_in_log node3
log 'format-2 mandatory request history fails closed before listeners and recovers retained records'
