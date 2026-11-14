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
# A lone frozen node cannot initialize without every configured member.
start_node node1
wait_for_socket node1
sleep .2
cmp "$(node_config node1).state/activation.json" "$TEST_TMP/node1-frozen.json" || die 'offline members did not block initialization'
[[ ! -e "$(node_config node1).state/requests.json" ]] || die 'lone frozen node initialized history'
start_node node2
start_node node3
frozen_status() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["authority"]=="json" and s["admitted"] and s["committed_revision"]==s["applied_revision"]==1
assert all(n["connected"] and n["metadata_known"] and n["protocol"]==6 for n in s["nodes"])
' 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 45 "$node frozen coordination startup" frozen_status "$node"
    "$LCS" -s "$(node_socket "$node")" --json status | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["cluster"]["quorum"]
r=s["resources"]; assert len(r)==1 and r[0]["address"]=="127.0.0.200/32"
'
    if "$LCS" -s "$(node_socket "$node")" reload >"$TEST_TMP/$node-reload.log" 2>&1; then die 'frozen node accepted INI reload'; fi
    "$LCS" -s "$(node_socket "$node")" config export >"$TEST_TMP/$node-export.json"
    python3 - "$TEST_TMP/$node-export.json" "$TEST_TMP/$node-frozen.json" <<'PY'
import json,sys
export=json.load(open(sys.argv[1])); frozen=json.loads(json.load(open(sys.argv[2]))['baseline'])
assert export['schema_version']==2 and export['revision']==1
for r in export['resources'].values(): assert r.pop('birth_revision')==0
export['schema_version']=1; export['revision']=0
assert export==frozen
PY
    python3 - "$(node_config "$node").state" "$TEST_TMP/$node-frozen.json" <<'PY'
import json,sys
from pathlib import Path
state=Path(sys.argv[1]); old=json.load(open(sys.argv[2])); current=json.loads((state/'activation.json').read_text())
assert current['record_version']==2 and current['phase']=='prepared'
assert all(old[k]==current[k] for k in ('member','membership','baseline_digest','baseline'))
assert any(r['committed'] and r['applied'] for r in json.loads((state/'requests.json').read_text())['requests'])
barrier=json.loads((state/'barrier.json').read_text())
assert barrier['ready_voters']==7 and barrier['membership']==current['membership']
PY
    cp "$(node_config "$node").state/activation.json" "$TEST_TMP/$node-prepared.json"
    cp "$(node_config "$node").state/barrier.json" "$TEST_TMP/$node-barrier.json"
done
# Exercise multiple peer timeouts: metadata keeps the coordination channel alive
# while resource heartbeats, votes and placement remain fenced.
sleep 2
for node in node1 node2 node3; do frozen_status "$node" || die 'frozen coordination channel lost liveness'; done
# Signals and restart retain the automatically activated catalog.
kill -HUP "${LCS_PIDS[0]}"
stop_node node3
start_node node3
LCS_PIDS[2]="${LCS_PIDS[3]}"
wait_until 45 'quorum-only frozen restart' frozen_status node3
for node in node1 node2 node3; do
    config="$(node_config "$node")"; state="$config.state"
    stop_node "$node"
    cmp "$state/activation.json" "$TEST_TMP/$node-prepared.json" || die 'restart or signal rewrote preparation'
    for record in agreement application requests; do [[ -e "$state/$record.json" ]] || die 'prepared history disappeared'; done
    cmp "$state/barrier.json" "$TEST_TMP/$node-barrier.json" || die 'restart rewrote durable readiness'
    printf '{}\n' >"$state/agreement.json"
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$config" --no-syslog --no-timestamp >"$TEST_TMP/$node-conflict.log" 2>&1; then die 'frozen runtime accepted contradictory history'; fi
    cmp "$state/activation.json" "$TEST_TMP/$node-prepared.json" || die 'conflicting history replaced preparation'
    [[ "$(cat "$state/agreement.json")" == '{}' ]] || die 'conflicting history was overwritten'
    grep -Fq 'cannot recover distributed resource configuration' "$TEST_TMP/$node-conflict.log" || die 'missing history recovery diagnostic'
done
assert_no_vip_ops_in_log node3
log 'durable freezes automatically initialize, confirm readiness and activate while preserving baselines across restart'
