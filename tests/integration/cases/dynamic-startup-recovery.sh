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
    "$ROOT_DIR/src/"{config_requests,config_activation,config_barrier,config_agreement,config_application,config_store,config_candidate,resource_config,config,util,source_allowlist}.c \
    "${fixture_flags[@]}" -o "$TEST_TMP/fixture"
for node in node1 node2 node3; do
    printf 'pre_stop = %s\npost_stop = %s\n' "$TEST_TMP/stop-hook" "$TEST_TMP/stop-hook" >>"$(node_config "$node")"
    "$TEST_TMP/fixture" "$(node_config "$node")"
    # Recover existing history on two members; the older catalog on node2 has
    # no request journal and must not acquire fabricated empty history.
    if [[ "$node" != node2 ]]; then
        "$TEST_TMP/fixture" "$(node_config "$node")" "$(if [[ "$node" == node1 ]]; then echo reserved; else echo history; fi)"
        cp "$(node_config "$node").state/requests.json" "$TEST_TMP/$node-requests.json"
    fi
    # Activated resource definitions must come from the journal, never edited INI.
    sed -i 's|127.0.0.200/32|127.0.0.203/32|' "$(node_config "$node")"
done
for node in node1 node3; do
    # Corruption must fail before opening a Unix listener, without resetting history.
    printf '{}\n' >"$(node_config "$node").state/requests.json"
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config "$node")" --no-syslog --no-timestamp >"$TEST_TMP/$node-invalid-history.log" 2>&1; then
        die 'daemon accepted corrupt request history'
    fi
    [[ ! -S "$(node_socket "$node")" ]] || die 'listener opened with corrupt request history'
    grep -Fq 'request history' "$TEST_TMP/$node-invalid-history.log" || die 'missing request recovery diagnostic'
    cp "$TEST_TMP/$node-requests.json" "$(node_config "$node").state/requests.json"
done
start_node node1
wait_for_socket node1
node_status_has node1 'vip1 127.0.0.201/32 dev=lo state=stopped owner=-' || die 'did not restore last applied definitions'
node_status_has node1 'quorum: no' || die 'isolated recovering node has quorum'
"$LCS" -s "$(node_socket node1)" --json config request-status recovery-3 | python3 -c '
import json, sys
s = json.load(sys.stdin)
assert s["scope"] == "local" and s["history"] == "available" and s["state"] == "committed"
assert s["base_revision"] == 2 and s["value_revision"] == 3 and len(s["value_digest"]) == 64
assert "applied" not in s and "success" not in s
'
"$LCS" -s "$(node_socket node1)" --json config request-status pending-request | python3 -c '
import json, sys
s = json.load(sys.stdin)
assert s["state"] == "reserved" and s["base_revision"] == 3 and s["value_revision"] == 4
'
"$LCS" -s "$(node_socket node1)" --json config request-status unknown-request | python3 -c '
import json, sys
s = json.load(sys.stdin)
assert s["state"] == "unknown" and s["history"] == "available" and s["base_revision"] is None
'

"$LCS" -s "$(node_socket node1)" config export | python3 -c 'import json, sys; assert json.load(sys.stdin)["revision"] == 3'
"$LCS" -s "$(node_socket node1)" config export applied | python3 -c 'import json, sys; s=json.load(sys.stdin); assert s["revision"] == 2 and s["resources"]["vip1"]["address"] == "127.0.0.201/32"'
if "$LCS" -s "$(node_socket node1)" resource start vip1 >"$TEST_TMP/mutation.log" 2>&1; then
    die 'recovery allowed a CLI mutation'
fi
grep -Fq 'recovery is active' "$TEST_TMP/mutation.log" || die 'missing recovery fence diagnostic'
"$LCS" -s "$(node_socket node1)" --json config status >"$TEST_TMP/config-status.json"
python3 - "$TEST_TMP/config-status.json" <<'PY'
import json, sys
status = json.load(open(sys.argv[1]))
assert status['authority'] == 'json' and status['dynamic_commands'] is False
assert status['admitted'] is False and status['application_state'] in ('recovering', 'blocked')
assert status['committed_revision'] == 3 and status['applied_revision'] == 2 and status['pending_revision'] == 3
assert len(status['committed_digest']) == 64 and len(status['applied_digest']) == 64
PY
sleep 1
! grep -q 'recovery admitted' "$TEST_TMP/logs/node1.log" || die 'admitted without other full member'
touch "$LCS_RECOVERY_TRACE.block"
start_node node2
start_node node3
admitted() { grep -Fq 'recovery admitted revision=3' "$TEST_TMP/logs/$1.log"; }
blocked() { grep -Fq 'distributed resource recovery blocked:' "$TEST_TMP/logs/node2.log"; }
wait_until 15 'failed cleanup blocks recovery' blocked
blocked_status() {
    "$LCS" -s "$(node_socket node2)" --json config status >"$TEST_TMP/blocked.json" &&
        python3 - "$TEST_TMP/blocked.json" 2>/dev/null <<'PY'
import json, sys
status = json.load(open(sys.argv[1]))
assert status['authority'] == 'json' and status['admitted'] is False
assert status['application_state'] == 'blocked' and status['error']
assert status['committed_revision'] == 3 and status['applied_revision'] == 1 and status['pending_revision'] == 2
PY
}
wait_until 10 'read-only cleanup failure diagnostics' blocked_status
! admitted node1 && ! admitted node2 || die 'admitted despite failed full-member cleanup'
rm "$LCS_RECOVERY_TRACE.block"
for node in node1 node2 node3; do
    wait_until 35 "$node dynamic admission" admitted "$node"
    wait_for_quorum "$node"
    "$LCS" -s "$(node_socket "$node")" --json config status >"$TEST_TMP/$node-status.json"
    python3 - "$TEST_TMP/$node-status.json" <<'PY'
import json, sys
status = json.load(open(sys.argv[1]))
assert status['authority'] == 'json' and status['dynamic_commands'] is ('node1-status' in sys.argv[1])
assert status['admitted'] is True and status['application_state'] == 'active'
assert status['committed_revision'] == status['applied_revision'] == 3
assert status['committed_digest'] == status['applied_digest']
assert status['pending_revision'] is None and status['pending_digest'] is None and status['error'] is None
PY
done
"$LCS" -s "$(node_socket node1)" config status | grep -Fq 'Resource configuration: JSON' || die 'missing JSON text diagnostics'
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

for node in node1 node3; do
    python3 - "$TEST_TMP/$node-requests.json" "$(node_config "$node").state/requests.json" <<'PYTEST'
import json, sys
old, new = (json.load(open(p)) for p in sys.argv[1:])
assert old['requests'][0]['applied'] is False and new['requests'][0]['applied'] is True
new['requests'][0]['applied'] = False
assert old['requests'] == new['requests']
PYTEST
    "$LCS" -s "$(node_socket "$node")" --json config request-status recovery-3 | python3 -c 'import json,sys; assert json.load(sys.stdin)["state"] == "applied"'
done
[[ ! -e "$(node_config node2).state/requests.json" ]] || die 'startup fabricated missing request history'
log 'activated daemon recovery and restart regression passed'
