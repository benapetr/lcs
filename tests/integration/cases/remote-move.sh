#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3
wait_for_owner node1 node1

log "moving vip1 from node1 to node2 through the deprecated CLI alias"
move_stderr="$TEST_TMP/move.stderr"
"$LCS" -s "$(node_socket node1)" move vip1 node2 2>"$move_stderr"
grep -Fq "'lcs move' is deprecated; use 'lcs resource move' instead" "$move_stderr" ||
    die "deprecated move alias did not print its warning"

grep -Fq "release quorum confirmed for resource vip1" "$TEST_TMP/logs/node1.log" ||
    die "old owner did not confirm a release quorum before completing move"

wait_for_owner node1 node2
wait_for_owner node2 node2
wait_for_owner node3 node2

log "remote move regression passed"
