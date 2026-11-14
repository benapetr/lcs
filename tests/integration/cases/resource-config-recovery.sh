#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077

expect_recovery_failure()
{
    local node="$1" expected="${2:-cannot recover distributed resource configuration}" code=0
    timeout 10s env LCS_VIP_DRY_RUN=1 LCS_SYSTEMD_DRY_RUN=1 "$LCSD" \
        -c "$(node_config "$node")" --no-syslog --no-timestamp \
        >"$TEST_TMP/recovery-failure.log" 2>&1 || code=$?
    [[ "$code" == 1 ]] || die "expected recovery exit 1, got $code"
    grep -Fq "$expected" "$TEST_TMP/recovery-failure.log" ||
        die "startup did not explain distributed recovery failure"
    [[ ! -e "$(node_socket "$node")" ]] || die "failed recovery exposed a CLI socket"
}

prepare_cluster
state="$(node_config node1).state"
snapshot="$(node_config node1).resources.json"
mkdir -m 0700 "$state"
log "checking orphan readiness blocks first INI import"
printf '{orphan' >"$state/barrier.json"
expect_recovery_failure node1
[[ ! -e "$snapshot" && ! -e "$state/bootstrap.state" && ! -e "$state/activation.json" ]] ||
    die "orphan barrier imported or initialized configuration"
rm "$state/barrier.json"
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
# Keep one pre-upgrade witness while testing legacy shadow/recovery records.
# Full members and the explicit recovery subprocesses use the production daemon.
LCSD="$(python3 "$ROOT_DIR/tests/integration/helpers/wire5_daemon.py")" start_node node3
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
log "checking an interrupted frozen initialization resumes without importing edited INI"
rm "$state/agreement.json"
"$LCS" -s "$(node_socket node1)" --json config status >"$TEST_TMP/prepared-status.json"
# Build an independent on-disk crash fixture from the real prepared snapshot.
# A prior coordinator is assumed to have durably authorized this exact intent;
# this is not a supported administrative method of starting activation.
python3 - "$state/activation.json" "$TEST_TMP/prepared-status.json" "$snapshot" <<'PYFIXTURE'
import hashlib, json, sys
from pathlib import Path
status = json.loads(Path(sys.argv[2]).read_text())
record = {"record_version": 1, "phase": "initializing", "member": "node2",
          "membership": status["membership"], "baseline_digest": status["baseline_digest"],
          "baseline": Path(sys.argv[3]).read_text()}
canonical = json.dumps(record, sort_keys=True, ensure_ascii=True, separators=(",", ":"))
record["checksum"] = hashlib.sha256(canonical.encode()).hexdigest()
Path(sys.argv[1]).write_text(json.dumps(record, sort_keys=True))
PYFIXTURE
printf '{premature' >"$state/barrier.json"
expect_recovery_failure node2
[[ ! -e "$state/agreement.json" && ! -e "$state/application.json" ]] ||
    die "premature barrier authorized initialization"
rm "$state/barrier.json"
expect_recovery_failure node2 "persisted distributed resource configuration requires activation-aware recovery"
python3 - "$state" <<'PYRECOVERY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
intent = json.loads((root / "activation.json").read_text())
assert intent["phase"] == "prepared"
agreement = json.loads((root / "agreement.json").read_text())
application = json.loads((root / "application.json").read_text())
assert agreement["promised"] == "0" and agreement["accepted"] is None
for snapshot in (agreement["committed"]["snapshot"], json.loads(application["applied"]["document"])):
    assert snapshot["revision"] == 0 and snapshot["resources"]["vip1"]["address"] == "127.0.0.200/32"
assert application["pending"] is None
assert all((root / name).stat().st_mode & 0o777 == 0o600
           for name in ("activation.json", "agreement.json", "application.json"))
PYRECOVERY
cmp "$snapshot" "$TEST_TMP/snapshot-before.json" || die "resuming initialization overwrote the frozen shadow"
cmp "$state/bootstrap.state" "$TEST_TMP/bootstrap-before.state" || die "initialization reset legacy metadata"
log "checking exact durable all-member readiness and invalid proof recovery"
for variant in partial foreign-member foreign-membership foreign-baseline version unknown checksum valid; do
    python3 - "$state" "$variant" <<'PYBARRIER'
import hashlib, json, sys
from pathlib import Path
root, variant = Path(sys.argv[1]), sys.argv[2]
intent = json.loads((root / "activation.json").read_text())
record = {"record_version": 1, "member": intent["member"],
          "membership": intent["membership"], "baseline_digest": intent["baseline_digest"],
          "ready_voters": 7}
if variant == "partial": record["ready_voters"] = 3
if variant == "foreign-member": record["member"] = "node1"
if variant == "foreign-membership": record["membership"] = "0" * 64
if variant == "foreign-baseline": record["baseline_digest"] = "0" * 64
if variant == "version": record["record_version"] = 2
if variant == "unknown": record["unexpected"] = True
canonical = json.dumps(record, sort_keys=True, ensure_ascii=True, separators=(",", ":"))
record["checksum"] = hashlib.sha256(canonical.encode()).hexdigest()
if variant == "checksum": record["checksum"] = "0" * 64
(root / "barrier.json").write_text(json.dumps(record, sort_keys=True))
PYBARRIER
    if [[ "$variant" == valid ]]; then
        cp "$state/barrier.json" "$TEST_TMP/barrier-before.json"
        expect_recovery_failure node2 "persisted distributed resource configuration requires activation-aware recovery"
        cmp "$state/barrier.json" "$TEST_TMP/barrier-before.json" || die "recovery rewrote durable readiness"
    else
        expect_recovery_failure node2
        grep -Fq "invalid, corrupt, or foreign durable barrier" "$TEST_TMP/recovery-failure.log" ||
            die "invalid readiness was not diagnosed: $variant"
    fi
    cmp "$snapshot" "$TEST_TMP/snapshot-before.json" || die "readiness recovery refreshed INI"
done
# Prepared recovery must not recreate a lost catalog or replace the baseline.
rm "$state/application.json"
expect_recovery_failure node2
grep -Fq "catalog missing" "$TEST_TMP/recovery-failure.log" || die "prepared catalog loss was not diagnosed"
[[ ! -e "$state/application.json" ]] || die "prepared recovery recreated a missing catalog"
cmp "$state/barrier.json" "$TEST_TMP/barrier-before.json" || die "catalog loss changed durable readiness"
wait_for_owner node1 node1
log "daemon configuration recovery regression passed"
