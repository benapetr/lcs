#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT

snapshot_has_address()
{
    python3 - "$(node_config "$1").resources.json" "$2" 2>/dev/null <<'PY'
import json
import sys
with open(sys.argv[1]) as source:
    snapshot = json.load(source)
assert snapshot['schema_version'] == 1
assert snapshot['revision'] == 0
assert snapshot['resources']['vip1']['address'] == sys.argv[2]
PY
}

start_cluster
for node in node1 node2 node3; do
    wait_for_socket "$node"
    wait_for_quorum "$node"
    snapshot_has_address "$node" 127.0.0.200/32
    [[ -f "$(node_config "$node").state/bootstrap.state" ]] ||
        die "$node did not persist bootstrap initialization"
done
wait_for_owner node1 node1

log "checking successful legacy reload refreshes every prepared JSON snapshot"
for node in node1 node2 node3; do
    sed -i 's|127.0.0.200/32|127.0.0.201/32|' "$(node_config "$node")"
done
"$LCS" -s "$(node_socket node1)" reload >/dev/null
for node in node1 node2 node3; do
    wait_until 15 "$node prepared JSON reflects committed INI reload" \
        snapshot_has_address "$node" 127.0.0.201/32
done
wait_until 15 "changed resource is active" node_status_has node1 \
    "vip1 127.0.0.201/32 dev=lo state=active owner=node1"

log "checking restart retains prepared state and missing JSON is not reimported"
stop_node node2
start_node node2
wait_for_socket node2
wait_for_quorum node2
snapshot_has_address node2 127.0.0.201/32
new_pid="${LCS_PIDS[${#LCS_PIDS[@]}-1]}"
kill "$new_pid"
wait "$new_pid" || true
rm "$(node_config node2).resources.json"
if env LCS_VIP_DRY_RUN=1 LCS_SYSTEMD_DRY_RUN=1 "$LCSD" \
    -c "$(node_config node2)" --no-syslog --no-timestamp \
    >"$TEST_TMP/missing-snapshot.log" 2>&1; then
    die "node2 silently reimported INI after its initialized JSON was deleted"
fi
grep -Fq "recover missing state" "$TEST_TMP/missing-snapshot.log" ||
    die "missing snapshot error did not explain recovery requirement"
[[ ! -e "$(node_config node2).resources.json" ]] ||
    die "node2 recreated missing initialized snapshot"
log "resource JSON bootstrap regression passed"
