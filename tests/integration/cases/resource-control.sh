#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

resource_disabled_stopped()
{
    status_text node1 2>/dev/null |
        grep -F "vip1 127.0.0.200/32 dev=lo state=stopped owner=-" |
        grep -Fq "disabled=yes"
}

resource_list_disabled()
{
    "$LCS" -s "$(node_socket node1)" resource list 2>/dev/null |
        grep -F "vip1 state=stopped owner=- address=127.0.0.200/32 dev=lo" |
        grep -Fq "disabled=yes"
}

resource_list_active()
{
    "$LCS" -s "$(node_socket node1)" resource list 2>/dev/null |
        grep -Fq "vip1 state=active owner=node1 address=127.0.0.200/32 dev=lo"
}

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3
wait_for_owner node1 node1
wait_until 8 "resource list active" resource_list_active

log "stopping vip1 through non-owner local CLI"
"$LCS" -s "$(node_socket node2)" resource stop vip1
wait_until 10 "vip1 stopped and disabled" resource_disabled_stopped
wait_until 8 "resource list disabled" resource_list_disabled

if "$LCS" -s "$(node_socket node1)" resource move vip1 node2 >"$TEST_TMP/move-disabled.out" 2>&1; then
    die "move of administratively stopped vip1 unexpectedly succeeded"
fi
grep -Fq "resource is administratively stopped" "$TEST_TMP/move-disabled.out" ||
    die "move of stopped vip1 did not report administrative stop"

sleep 2
resource_disabled_stopped || die "vip1 did not remain administratively stopped"

log "starting vip1 again"
"$LCS" -s "$(node_socket node3)" resource start vip1
wait_for_owner node1 node1
wait_until 8 "resource list active after start" resource_list_active

log "resource control regression passed"
