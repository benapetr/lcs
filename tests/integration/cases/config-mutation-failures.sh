#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077
prepare_cluster
export LCS_VIP_FAIL_DEL_FILE="$TEST_TMP/fail-delete"
read -r -a fixture_flags <<<"$(pkg-config --cflags --libs jansson libcrypto)"
"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 -Wall -Wextra -Wpedantic \
    "$SCRIPT_DIR/../helpers/config_recovery_fixture.c" \
    "$ROOT_DIR/src/"{config_requests,config_activation,config_barrier,config_agreement,config_application,config_store,config_candidate,resource_config,config,util,source_allowlist}.c \
    "${fixture_flags[@]}" -o "$TEST_TMP/fixture"
for node in node1 node2 node3; do
    "$TEST_TMP/fixture" "$(node_config "$node")"
    "$TEST_TMP/fixture" "$(node_config "$node")" history
    start_node "$node"
done
admitted_revision() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["admitted"] and s["committed_revision"]==s["applied_revision"]==int(sys.argv[1])
' "$2" 2>/dev/null
}
for node in node1 node2 node3; do wait_until 40 "$node failure-test startup" admitted_revision "$node" 3; done
wait_until 15 'initial VIP active' node_status_has node1 'vip1 127.0.0.202/32 dev=lo state=active owner='
if ! node_status_has node1 'vip1 127.0.0.202/32 dev=lo state=active owner=node1'; then
    "$LCS" -s "$(node_socket node1)" resource move vip1 node1
fi
wait_until 15 'origin owns VIP' node_status_has node1 'vip1 127.0.0.202/32 dev=lo state=active owner=node1'
touch "$LCS_VIP_FAIL_DEL_FILE"
for attempt in {1..20}; do
    result=0
    "$LCS" -s "$(node_socket node1)" --json --request-id failed-stop resource update vip1 address=127.0.0.203/32 >"$TEST_TMP/stop-result.json" || result=$?
    (( result != 1 )) && break
    grep -Eq 'busy|not admitted' "$TEST_TMP/stop-result.json" || die 'stop-test submission rejected'
    sleep .2
done
(( result == 2 )) || die "stop failure lost pending decision (exit $result)"
python3 - "$TEST_TMP/stop-result.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='pending' and s['committed_revision']==4 and s['applied_revision'] is None,s
assert 'vip1 cleanup failed' in s['reason'] and 'restart' in s['reason'] and 'config status' in s['reason'],s
PY
result=0
"$LCS" -s "$(node_socket node1)" --json --request-id failed-stop resource update vip1 address=127.0.0.203/32 >"$TEST_TMP/stop-retry.json" || result=$?
(( result == 2 )) || die 'exact failed-stop retry changed outcome'
grep -Fq 'vip1 cleanup failed' "$TEST_TMP/stop-retry.json" || die 'exact retry omitted backend failure'
result=0
"$LCS" -s "$(node_socket node1)" --json --request-id while-blocked resource update vip1 priority=77 >"$TEST_TMP/fenced.json" || result=$?
(( result == 1 )) || die 'new edit passed failed cleanup fence'
grep -Fq 'vip1 cleanup failed' "$TEST_TMP/fenced.json" || die 'new submission hid cleanup fence cause'
python3 - "$(node_config node1).state" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]); a=json.loads((p/'application.json').read_text()); h=json.loads((p/'requests.json').read_text())
r=next(r for r in h['requests'] if r['request_id']=='failed-stop')
assert r['committed'] and not r['applied'] and a['pending'] is not None
assert all(r['request_id']!='while-blocked' for r in h['requests'])
PY
# Repair is followed by fresh restart cleanup, never a timer clearing stop proof.
rm "$LCS_VIP_FAIL_DEL_FILE"
for node in node1 node2 node3; do stop_node "$node"; done
for node in node1 node2 node3; do
    start_node "$node"
    LCS_PIDS[$(( ${node#node} - 1 ))]="${LCS_PIDS[-1]}"
done
for node in node1 node2 node3; do wait_until 40 "$node repaired stop admission" admitted_revision "$node" 4; done
# Force the originating reservation write to fail without touching its agreement.
state="$(node_config node1).state"
cp "$state/agreement.json" "$TEST_TMP/agreement-before.json"
mv "$state/requests.json" "$TEST_TMP/requests-before.json"
ln -s "$state/agreement.json" "$state/requests.json"
for attempt in {1..20}; do
    result=0
    "$LCS" -s "$(node_socket node1)" --json --request-id failed-storage resource update vip1 priority=88 >"$TEST_TMP/storage-result.json" || result=$?
    (( result != 1 )) && break
    grep -Eq 'busy|not admitted' "$TEST_TMP/storage-result.json" || die 'storage-test submission rejected'
    sleep .2
done
(( result == 3 )) || die "uncertain storage failure did not stay unknown (exit $result)"
python3 - "$TEST_TMP/storage-result.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='unknown' and s['committed_revision'] is None and s['applied_revision'] is None,s
assert 'configuration store' in s['reason'] and 'Retain state' in s['reason'] and 'restart' in s['reason'],s
PY
result=0
"$LCS" -s "$(node_socket node1)" --json --request-id failed-storage resource update vip1 priority=88 >"$TEST_TMP/storage-retry.json" || result=$?
(( result == 3 )) || die 'unusable history permitted retry execution'
# Fatal storage failure shuts the daemon down; a subsequent disconnected retry
# can only retain its ID and unknown result, not invent a readable history.
python3 - "$TEST_TMP/storage-retry.json" <<'PYTHON'
import json,sys
s=json.load(open(sys.argv[1])); assert s['request_id']=='failed-storage' and s['result']=='unknown',s
PYTHON
cmp "$state/agreement.json" "$TEST_TMP/agreement-before.json" || die 'failed reservation reached PREPARE or changed agreement'
for node in node1 node2 node3; do stop_node "$node"; done
rm "$state/requests.json"
mv "$TEST_TMP/requests-before.json" "$state/requests.json"
log 'failed-stop pending diagnostics, verified restart repair and uncertain storage recovery diagnostics passed'
