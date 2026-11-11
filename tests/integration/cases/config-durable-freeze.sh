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
    # A restart must neither thaw nor create journals. Live startup resumption
    # remains unavailable, so the current daemon refuses before opening sockets.
    for attempt in 1 2; do
        if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$config" --no-syslog --no-timestamp >"$TEST_TMP/$node-frozen.log" 2>&1; then
            die 'unactivated frozen runtime silently fell back to INI'
        fi
        [[ ! -S "$(node_socket "$node")" ]] || die 'frozen runtime opened listener before activation-aware recovery'
        cmp "$state/activation.json" "$TEST_TMP/$node-frozen.json" || die 'startup changed freeze without all-member authorization'
        for record in agreement application requests barrier; do
            [[ ! -e "$state/$record.json" ]] || die 'freeze alone created initialization history'
        done
        grep -Fq 'requires activation-aware recovery' "$TEST_TMP/$node-frozen.log" || die 'missing frozen startup recovery diagnostic'
    done
    # Contradictory history must fail before any attempted promotion/overwrite.
    printf '{}\n' >"$state/agreement.json"
    if LCS_VIP_DRY_RUN=1 "$LCSD" -c "$config" --no-syslog --no-timestamp >"$TEST_TMP/$node-conflict.log" 2>&1; then
        die 'frozen runtime accepted contradictory agreement history'
    fi
    cmp "$state/activation.json" "$TEST_TMP/$node-frozen.json" || die 'conflicting history replaced freeze'
    [[ "$(cat "$state/agreement.json")" == '{}' ]] || die 'conflicting history was overwritten'
    grep -Fq 'existing agreement/application/request history' "$TEST_TMP/$node-conflict.log" || die 'missing freeze/history conflict diagnostic'
done
log 'durable freeze survives restart without promotion or fallback, including quorum-only members'
