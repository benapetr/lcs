#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077

expect_recovery_failure()
{
    local node="$1" code=0
    timeout 10s env LCS_VIP_DRY_RUN=1 LCS_SYSTEMD_DRY_RUN=1 "$LCSD" \
        -c "$(node_config "$node")" --no-syslog --no-timestamp \
        >"$TEST_TMP/recovery-failure.log" 2>&1 || code=$?
    [[ "$code" == 1 ]] || die "expected recovery exit 1, got $code"
    grep -Fq "cannot recover distributed resource configuration" "$TEST_TMP/recovery-failure.log" ||
        die "startup did not explain distributed recovery failure"
    [[ ! -e "$(node_socket "$node")" ]] || die "failed recovery exposed a CLI socket"
}

prepare_cluster
state="$(node_config node1).state"
snapshot="$(node_config node1).resources.json"
mkdir -m 0700 "$state"
log "checking corrupt durable history blocks first INI import"
printf '{broken' >"$state/agreement.json"
expect_recovery_failure node1
[[ ! -e "$snapshot" && ! -e "$state/bootstrap.state" ]] ||
    die "failed recovery imported INI or initialized bootstrap metadata"
[[ "$(cat "$state/agreement.json")" == '{broken' ]] ||
    die "failed recovery changed the journal"
rm "$state/agreement.json"

log "checking an orphan application catalog never recreates missing agreement"
printf '{orphan' >"$state/application.json"
expect_recovery_failure node1
grep -Fq "journal is missing" "$TEST_TMP/recovery-failure.log" ||
    die "missing agreement diagnostic did not require recovery"
[[ ! -e "$state/agreement.json" && ! -e "$snapshot" ]] ||
    die "startup silently recreated distributed state"
rm "$state/application.json"

log "checking legacy startup still works without distributed state"
start_node node1
start_node node2
start_node node3
for node in node1 node2 node3; do
    wait_for_socket "$node"
    wait_for_quorum "$node"
done
wait_for_owner node1 node1
stop_node node2
state="$(node_config node2).state"
snapshot="$(node_config node2).resources.json"
cp "$snapshot" "$TEST_TMP/snapshot-before.json"
cp "$state/bootstrap.state" "$TEST_TMP/bootstrap-before.state"
sed -i 's|127.0.0.200/32|127.0.0.202/32|' "$(node_config node2)"
printf '{broken' >"$state/agreement.json"
log "checking recovery precedes refresh of an existing INI shadow"
expect_recovery_failure node2
cmp "$snapshot" "$TEST_TMP/snapshot-before.json" || die "recovery failure overwrote the old snapshot"
cmp "$state/bootstrap.state" "$TEST_TMP/bootstrap-before.state" || die "recovery failure changed initialization state"
wait_for_owner node1 node1
log "daemon configuration recovery regression passed"
