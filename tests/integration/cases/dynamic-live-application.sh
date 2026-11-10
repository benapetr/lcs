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
for node in node1 node2 node3; do "$TEST_TMP/fixture" "$(node_config "$node")"; done
# Track learned outcomes on node1 while preserving missing-history compatibility on node2.
"$TEST_TMP/fixture" "$(node_config node1)" history
start_node node1
start_node node2
start_node node3
admitted_revision() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json, sys
s = json.load(sys.stdin)
assert s["authority"] == "json" and s["admitted"] and s["dynamic_commands"] is False
assert s["committed_revision"] == s["applied_revision"] == int(sys.argv[1])
assert s["pending_revision"] is None
' "$2" 2>/dev/null
}
for node in node1 node2 node3; do wait_until 40 "$node startup admission" admitted_revision "$node" 3; done
wait_until 15 'initial latest VIP placement' node_status_has node1 'vip1 127.0.0.202/32 dev=lo state=active owner='
full1="${LCS_PIDS[0]}" full2="${LCS_PIDS[1]}"
advance_witness() {
    stop_node node3
    "$TEST_TMP/fixture" "$(node_config node3)" advance
    start_node node3
    LCS_PIDS[2]="${LCS_PIDS[-1]}"
}

# Both full members remain running. The stopped witness supplies a validated
# checkpoint through real wire-6 catch-up; there is no test/public mutation API.
snapshot="$(node_config node2).resources.json"
mv "$snapshot" "$TEST_TMP/old-projection.json"
ln -s "$(node_config node2).state/agreement.json" "$snapshot"
advance_witness
blocked_projection() {
    "$LCS" -s "$(node_socket node2)" --json config status 2>/dev/null | python3 -c '
import json, sys
s = json.load(sys.stdin)
assert s["committed_revision"] == 4 and s["applied_revision"] == 3 and s["pending_revision"] == 4
assert s["application_state"] == "blocked" and not s["admitted"] and s["error"]
' 2>/dev/null
}
wait_until 25 'live projection failure keeps application fenced' blocked_projection
"$LCS" -s "$(node_socket node2)" config export >"$TEST_TMP/committed-export.json"
"$LCS" -s "$(node_socket node2)" config export applied >"$TEST_TMP/applied-export.json"
python3 - "$TEST_TMP" <<'PY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
committed = json.loads((root/'committed-export.json').read_text())
applied = json.loads((root/'applied-export.json').read_text())
assert committed['revision'] == 4 and committed['resources']['vip1']['address'] == '127.0.0.203/32'
assert applied['revision'] == 3 and applied['resources']['vip1']['address'] == '127.0.0.202/32'
PY
if "$LCS" -s "$(node_socket node2)" resource stop vip1 >"$TEST_TMP/cli-error.log" 2>&1; then
    die 'live application allowed CLI mutation'
fi
grep -Fq 'application is active' "$TEST_TMP/cli-error.log" || die 'missing application fence diagnostic'
! node_status_has node1 'vip1 127.0.0.203/32 dev=lo state=active' || die 'replacement activated before owner proof'
if [[ "${LCS_IT_RESTART_DURING_APPLICATION:-0}" == 1 ]]; then
    # Exercise cancellation/close with a saved pending application, then recover
    # it while the other full member retains its live pinned owner receipt.
    stop_node node2
    grep -Fq 'lcsd stopped' "$TEST_TMP/logs/node2.log" || die 'blocked application did not shut down cleanly'
    rm "$snapshot"
    start_node node2
    LCS_PIDS[1]="${LCS_PIDS[-1]}"
    full2="${LCS_PIDS[1]}"
    wait_for_socket node2
else
    rm "$snapshot"
fi
for revision in 4 5 6 7 8; do
    if (( revision > 4 )); then advance_witness; fi
    for node in node1 node2 node3; do
        wait_until 40 "$node live revision $revision" admitted_revision "$node" "$revision"
        wait_for_quorum "$node"
    done
    kill -0 "$full1" && kill -0 "$full2" || die 'live application restarted a full member'
    if (( revision < 8 )); then
        wait_until 15 "vip1 placement at revision $revision" node_status_has node1 'vip1 127.0.0.203/32 dev=lo state=active owner='
    fi
    if (( revision == 5 || revision == 6 )); then
        wait_until 15 'created vip2 placement' node_status_has node1 'vip2 127.0.0.204/32 dev=lo state=active owner='
    fi
    for node in node1 node2 node3; do
        python3 - "$(node_config "$node")" "$revision" <<'PY'
import json, sys
from pathlib import Path
root, revision = sys.argv[1], int(sys.argv[2])
p = json.loads(Path(root + '.resources.json').read_text())
a = json.loads(Path(root + '.state/application.json').read_text())
assert p['revision'] == revision and json.loads(a['applied']['document']) == p and a['pending'] is None
assert len(p['resources']) == (0 if revision == 8 else 2 if revision in (5, 6) else 1)
history_path = Path(root + '.state/requests.json')
if history_path.exists():
    history = json.loads(history_path.read_text())
    record = next(r for r in history['requests'] if r['request_id'] == 'recovery-' + str(revision))
    assert record['committed'] is True and record['base_revision'] == revision - 1
    assert len(record['value_digest']) == 64

if revision < 8:
    assert p['resources']['vip1']['birth_revision'] == 4
if revision == 6:
    assert p['resources']['vip1']['priority'] == 66
PY
    done
done
assert_no_vip_ops_in_log node3
for node in node1 node2; do
    grep -Fq 'distributed resource application admitted revision=8' "$TEST_TMP/logs/$node.log" || die 'missing live admission log'
    ! grep -Fq 'distributed resource manager failed:' "$TEST_TMP/logs/$node.log" || die 'live manager failed'
    "$LCS" -s "$(node_socket "$node")" --json status | python3 -c 'import json, sys; assert json.load(sys.stdin)["resources"] == []'
    "$LCS" -s "$(node_socket "$node")" config export | python3 -c 'import json, sys; s=json.load(sys.stdin); assert s["revision"] == 8 and s["resources"] == {}'
done
log 'live replacement, creation, policy update and deletion regression passed'
