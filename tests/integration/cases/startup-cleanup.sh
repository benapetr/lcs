#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster
for node in node1 node2 node3; do
    cat >>"$(node_config "$node")" <<EOF

[service app]
systemd_unit = app.service
EOF
done

fail_file="$TEST_TMP/fail-systemd-stop"
touch "$fail_file"
export LCS_EXTRA_ENV_node1="LCS_SYSTEMD_FAIL_STOP_FILE=$fail_file"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3

wait_until 8 "startup service cleanup failure on node1" \
    node_status_has node1 "app type=service unit=app.service state=stop_failed owner=node1"
node_status_has node1 "node1 role=full-member state=recovering (self)" ||
    die "node1 became voting-ready while startup cleanup was failing"
wait_until 8 "startup cleanup failure propagated to peers" \
    node_status_has node2 "app type=service unit=app.service state=stop_failed owner=node1"

sleep 2.5
node_status_has node1 "node1 role=full-member state=recovering (self)" ||
    die "node1 left recovery while startup cleanup was still failing"
if grep -Fq "activated service app" "$TEST_TMP/logs/node1.log" "$TEST_TMP/logs/node2.log"; then
    die "service activated while startup cleanup could not prove it inactive"
fi

log "allowing startup service cleanup retry to succeed"
rm -f "$fail_file"
wait_until 8 "successful startup cleanup retry" \
    grep -Fq "startup cleanup recovered for service app" "$TEST_TMP/logs/node1.log"
wait_for_quorum node1
wait_until 12 "service activation after cleanup recovery" \
    node_status_has node1 "app type=service unit=app.service state=active owner=node1"

grep -Fq "startup cleanup verified service app inactive" "$TEST_TMP/logs/node2.log" ||
    die "successful systemd startup cleanup was not performed"

log "startup resource cleanup regression passed"
