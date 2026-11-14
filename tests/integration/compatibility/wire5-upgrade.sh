#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
: "${LCS_WIRE5_DAEMON:?Set LCS_WIRE5_DAEMON to a pre-wire-6 lcsd accepting the integration config (for example dd78016).}"
[[ -x "$LCS_WIRE5_DAEMON" ]] || die "wire-5 daemon is not executable"
LCS_WIRE5_HAS_ALLOW_FROM="${LCS_WIRE5_HAS_ALLOW_FROM:-1}"
[[ "$LCS_WIRE5_HAS_ALLOW_FROM" == 0 || "$LCS_WIRE5_HAS_ALLOW_FROM" == 1 ]] || die "LCS_WIRE5_HAS_ALLOW_FROM must be 0 or 1"
trap cleanup_cluster EXIT

check_members()
{
    local expected="$1" protocol="$2"
    "$LCS" -s "$(node_socket node1)" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["matching_members"] == int(sys.argv[1])
assert s["dynamic_commands"] is False and s["activation_ready"] is False
assert s["authority"] == "ini" and s["configured_members"] == 3
' "$expected" 2>/dev/null || return 1
    "$LCS" -s "$(node_socket node1)" --json status | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["cluster"]["effective_protocol"] == int(sys.argv[1])
assert s["resources"][0]["owner"] == "node1"
' "$protocol" 2>/dev/null
}

prepare_cluster
if [[ "$LCS_WIRE5_HAS_ALLOW_FROM" == 0 ]]; then
    for node in node2 node3; do
        sed -i '/^allow_from = /d' "$(node_config "$node")"
    done
fi
start_node node1
LCSD="$LCS_WIRE5_DAEMON" start_node node2
LCSD="$LCS_WIRE5_DAEMON" start_node node3
for node in node1 node2 node3; do
    wait_for_socket "$node"
    wait_for_quorum "$node"
done
wait_for_owner node1 node1
if [[ "$LCS_WIRE5_HAS_ALLOW_FROM" == 0 ]]; then
    for node in node2 node3; do
        [[ ! -e "$(node_config "$node").resources.json" && ! -d "$(node_config "$node").state" ]] ||
            die "released legacy member unexpectedly prepared dynamic state"
    done
fi
wait_until 10 "legacy peers remain usable without new metadata" check_members 1 5
"$LCS" -s "$(node_socket node1)" --json config status | python3 -c '
import json,sys
s=json.load(sys.stdin)
for n in s["nodes"]:
    if not n["self"]:
        assert n["protocol"] == 5 and n["connected"] is True
        assert n["metadata_known"] is False and n["prepared"] is None
'
if "$LCS" -s "$(node_socket node2)" config status >"$TEST_TMP/old-command.log" 2>&1; then
    die "old daemon unexpectedly accepted the new local command"
fi
grep -Fq 'unsupported local CLI message' "$TEST_TMP/old-command.log" || die "new CLI did not explain old-daemon rejection"
initial_epoch="$("$LCS" -s "$(node_socket node1)" --json status | python3 -c 'import json,sys;print(json.load(sys.stdin)["resources"][0]["epoch"])')"

log "upgrading the other full member while retaining wire-5 quorum-only peering"
stop_node node2
if [[ "$LCS_WIRE5_HAS_ALLOW_FROM" == 0 ]]; then
    sed -i '/^\[cluster\]$/a allow_from = 127.0.0.0/8' "$(node_config node2)"
fi
start_node node2
wait_for_socket node2
wait_for_quorum node2
wait_until 10 "two matching new members with effective wire 5" check_members 2 5
# Prepared metadata is not restart-recovery/lease-voting readiness. Keep the
# old quorum-only voter online until the upgraded full member can actually vote.
wait_until 10 "upgraded full member is eligible to vote" node_status_has node1 "node2 role=full-member state=online"

# Until the final member upgrades, the active INI lease is unchanged and no
# durable freeze may be created.
"$LCS" -s "$(node_socket node1)" --json status | python3 -c '
import json,sys
r=json.load(sys.stdin)["resources"][0]
assert r["owner"]=="node1" and r["epoch"]==int(sys.argv[1])
' "$initial_epoch"
for node in node1 node2; do
    [[ ! -e "$(node_config "$node").state/activation.json" ]] || die 'mixed cluster froze before witness upgrade'
done
log "upgrading the quorum-only member and negotiating wire 6"
stop_node node3
if [[ "$LCS_WIRE5_HAS_ALLOW_FROM" == 0 ]]; then
    sed -i '/^\[cluster\]$/a allow_from = 127.0.0.0/8' "$(node_config node3)"
fi
start_node node3
wait_for_socket node3
activated_status()
{
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin)
assert s["authority"]=="json" and s["admitted"]
assert s["dynamic_commands"] == (next(n for n in s["nodes"] if n["self"])["role"] != "quorum-only")
assert s["committed_revision"]==s["applied_revision"]==1
assert all(n["connected"] and n["protocol"]==6 for n in s["nodes"])
' 2>/dev/null
}
for node in node1 node2 node3; do
    wait_until 45 "$node automatic migration after final upgrade" activated_status "$node"
    wait_for_quorum "$node"
done
wait_for_owner node1 node1
assert_no_vip_ops_in_log node3
if grep -Eq 'unsupported peer message|message requires a newer negotiated protocol|protocol error' "$TEST_TMP"/logs/*.log; then
    die "a new message escaped onto a legacy session or a legacy HELLO was broken"
fi
log "wire-5 to wire-6 rolling compatibility regression passed"
