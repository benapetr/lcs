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
"$TEST_TMP/fixture" "$(node_config node1)" activation-accepted
"$TEST_TMP/fixture" "$(node_config node2)" activation-reserved
"$TEST_TMP/fixture" "$(node_config node3)" activation-chosen
start_node node1
start_node node2
frozen() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="ini" and s["application_state"]=="frozen" and not s["admitted"] and not s["dynamic_commands"]
' 2>/dev/null
}
for node in node1 node2; do wait_until 10 "$node frozen before learning activation" frozen "$node"; done
first_pid="${LCS_PIDS[0]}"; second_pid="${LCS_PIDS[1]}"
start_node node3
admitted() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="json" and s["admitted"] and s["committed_revision"]==s["applied_revision"]==int(sys.argv[1])
' "${2:-1}" 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 40 "$node live activation admission" admitted "$node"
    wait_for_quorum "$node"
    "$LCS" -s "$(node_socket "$node")" --json config request-status interrupted-activation | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"]=="applied"'
done
kill -0 "$first_pid" "$second_pid"
for node in node1 node2; do
    grep -Fq 'chosen resource activation handed to recovery without restart' "$TEST_TMP/logs/$node.log" || die 'missing live handoff'
done
# The handoff must transfer the controller hold to normal majority-based edits.
stop_node node3
for attempt in {1..20}; do
    result=0
    "$LCS" -s "$(node_socket node1)" --json --request-id live-activation-edit --expected-revision 1 resource update vip1 priority=23 >"$TEST_TMP/edit.json" || result=$?
    (( result != 1 )) && break
    grep -Eq 'busy|not admitted' "$TEST_TMP/edit.json" || die 'post-activation mutation rejected'
    sleep .2
done
(( result != 1 )) || die 'live application did not admit majority-based mutation'
for node in node1 node2; do wait_until 35 "$node post-activation edit" admitted "$node" 2; done
start_node node3
LCS_PIDS[2]="${LCS_PIDS[-1]}"
wait_until 40 'quorum-only catch-up after live activation' admitted node3 2
assert_no_vip_ops_in_log node3
log 'live learned activation preserves daemon instances and enables majority-based edits after admission'
