#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

run_bad_config()
{
    local name="$1"
    local expected="$2"
    local cfg="$TEST_TMP/$name.conf"
    shift 2
    cat >"$cfg"
    local out="$TEST_TMP/$name.out"
    if "$LCSD" -c "$cfg" -f --no-syslog --no-timestamp >"$out" 2>&1; then
        cat "$out" >&2
        die "$name unexpectedly passed validation"
    fi
    grep -Fq "$expected" "$out" || {
        cat "$out" >&2
        die "$name did not report expected error: $expected"
    }
}

prepare_cluster

run_bad_config duplicate-priority "duplicate vip priority in group" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[group service]
type = keep-together
mode = strict

[vip vip1]
group = service
priority = 1
address = 127.0.0.201/32
interface = lo

[vip vip2]
group = service
priority = 1
address = 127.0.0.202/32
interface = lo
EOF

run_bad_config unknown-group "vip references unknown group" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[vip vip1]
group = missing
address = 127.0.0.201/32
interface = lo
EOF

log "group config validation regression passed"
