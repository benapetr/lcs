#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077
prepare_cluster
cat >"$TEST_TMP/stop-hook" <<'HOOK'
#!/bin/sh
printf '%s|%s|%s|%s|%s\n' "$LCS_NODE" "$LCS_EVENT" "$LCS_CONFIG_REVISION" "$LCS_RECOVERY" "$LCS_ADDRESS" >> "$LCS_RECOVERY_TRACE"
if [ "$LCS_NODE" = node2 ] && [ -e "$LCS_RECOVERY_TRACE.block" ]; then exit 1; fi
HOOK
chmod 0700 "$TEST_TMP/stop-hook"
export LCS_RECOVERY_TRACE="$TEST_TMP/cleanup.trace"
read -r -a fixture_flags <<<"$(pkg-config --cflags --libs jansson libcrypto)"
"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 -Wall -Wextra -Wpedantic \
    "$SCRIPT_DIR/../helpers/config_recovery_fixture.c" \
    "$ROOT_DIR/src/"{config_activation,config_barrier,config_agreement,config_application,config_store,config_candidate,resource_config,config,util,source_allowlist}.c \
    "${fixture_flags[@]}" -o "$TEST_TMP/fixture"
for node in node1 node2 node3; do
    printf 'pre_stop = %s\npost_stop = %s\n' "$TEST_TMP/stop-hook" "$TEST_TMP/stop-hook" >>"$(node_config "$node")"
    "$TEST_TMP/fixture" "$(node_config "$node")"
    # Activated resource definitions must come from the journal, never edited INI.
    sed -i 's|127.0.0.200/32|127.0.0.203/32|' "$(node_config "$node")"
done
start_node node1
wait_for_socket node1
node_status_has node1 'vip1 127.0.0.201/32 dev=lo state=stopped owner=-' || die 'did not restore last applied definitions'
node_status_has node1 'quorum: no' || die 'isolated recovering node has quorum'
if "$LCS" -s "$(node_socket node1)" resource start vip1 >"$TEST_TMP/mutation.log" 2>&1; then
    die 'recovery allowed a CLI mutation'
fi
grep -Fq 'recovery is active' "$TEST_TMP/mutation.log" || die 'missing recovery fence diagnostic'
if "$LCS" -s "$(node_socket node1)" --json config status >"$TEST_TMP/config-status.log" 2>&1; then
    die 'dynamic config status falsely reported legacy authority'
fi
grep -Fq 'use status' "$TEST_TMP/config-status.log" || die 'missing dynamic diagnostic limitation'
sleep 1
! grep -q 'recovery admitted' "$TEST_TMP/logs/node1.log" || die 'admitted without other full member'
touch "$LCS_RECOVERY_TRACE.block"
start_node node2
start_node node3
admitted() { grep -Fq 'recovery admitted revision=3' "$TEST_TMP/logs/$1.log"; }
blocked() { grep -Fq 'distributed resource recovery blocked:' "$TEST_TMP/logs/node2.log"; }
wait_until 15 'failed cleanup blocks recovery' blocked
! admitted node1 && ! admitted node2 || die 'admitted despite failed full-member cleanup'
rm "$LCS_RECOVERY_TRACE.block"
for node in node1 node2 node3; do
    wait_until 35 "$node dynamic admission" admitted "$node"
    wait_for_quorum "$node"
done
wait_until 15 'latest VIP placement' node_status_has node1 'vip1 127.0.0.202/32 dev=lo state=active owner='
for node in node1 node2 node3; do
    python3 - "$(node_config "$node")" <<'PY'
import json, sys
from pathlib import Path
config = Path(sys.argv[1])
projection = json.loads(Path(str(config) + '.resources.json').read_text())
assert projection['revision'] == 3
assert projection['resources']['vip1']['address'] == '127.0.0.202/32'
catalog = json.loads(Path(str(config) + '.state/application.json').read_text())
assert json.loads(catalog['applied']['document'])['revision'] == 3 and catalog['pending'] is None
PY
done
assert_no_vip_ops_in_log node3
grep -Fq '|1|127.0.0.200/32' "$LCS_RECOVERY_TRACE" || die 'retained backend cleanup was skipped'
grep -Fq '|1|127.0.0.201/32' "$LCS_RECOVERY_TRACE" || die 'applied backend cleanup was skipped'
! grep -Fq '127.0.0.203/32' "$LCS_RECOVERY_TRACE" || die 'cleanup used edited INI definitions'
! grep -q '^node3|' "$LCS_RECOVERY_TRACE" || die 'quorum-only node ran cleanup hooks'
stop_node node2
rm "$(node_config node2).resources.json"
start_node node2
wait_for_socket node2
wait_until 35 'node2 admission after restart with missing projection' admitted node2
wait_for_quorum node2
node_status_has node2 'vip1 127.0.0.202/32 dev=lo' || die 'restart loaded stale INI definitions'
[[ -f "$(node_config node2).resources.json" ]] || die 'restart did not restore projection'
log 'activated daemon recovery and restart regression passed'
