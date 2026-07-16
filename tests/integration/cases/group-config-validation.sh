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
    if "$LCSD" -c "$cfg" --no-syslog --no-timestamp >"$out" 2>&1; then
        cat "$out" >&2
        die "$name unexpectedly passed validation"
    fi
    grep -Fq "$expected" "$out" || {
        cat "$out" >&2
        die "$name did not report expected error: $expected"
    }
}

run_good_config_starts()
{
    local name="$1"
    local expected="$2"
    local expected2="$3"
    local cfg="$TEST_TMP/$name.conf"
    shift 3
    cat >"$cfg"
    local out="$TEST_TMP/$name.out"
    LCS_VIP_DRY_RUN=1 "$LCSD" -c "$cfg" --no-syslog --no-timestamp >"$out" 2>&1 &
    local pid=$!
    sleep 0.4
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null || true
        cat "$out" >&2
        die "$name unexpectedly failed validation/startup"
    fi
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    grep -Fq "$expected" "$out" || {
        cat "$out" >&2
        die "$name did not print expected output: $expected"
    }
    grep -Fq "$expected2" "$out" || {
        cat "$out" >&2
        die "$name did not print expected output: $expected2"
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
priority = 2
address = 127.0.0.201/32
interface = lo

[vip vip2]
group = service
priority = 2
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

run_bad_config unknown-home-node "vip references unknown home node" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[vip vip1]
home_node = missing
address = 127.0.0.201/32
interface = lo
EOF

run_bad_config quorum-only-home-node "vip home node must be a full-member" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[node node2]
role = quorum-only
address = 127.0.0.1

[vip vip1]
home_node = node2
address = 127.0.0.201/32
interface = lo
EOF

run_bad_config service-missing-unit "service systemd_unit must be a valid .service unit name" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[service app]
priority = 1
EOF

run_bad_config service-vip-fields "service resources cannot set address or interface" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[service app]
systemd_unit = app.service
address = 127.0.0.201/32
EOF

run_bad_config unknown-dependency "resource references unknown dependency" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[vip vip1]
depends_on = missing
address = 127.0.0.201/32
interface = lo
EOF

run_bad_config self-dependency "resource cannot depend on itself" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[vip vip1]
depends_on = vip1
address = 127.0.0.201/32
interface = lo
EOF

run_bad_config dependency-cycle "resource dependency cycle detected" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[vip vip1]
depends_on = vip2
address = 127.0.0.201/32
interface = lo

[vip vip2]
depends_on = vip1
address = 127.0.0.202/32
interface = lo
EOF

run_good_config_starts service-resource-config \
    "startup config:" \
    "vips=1" <<EOF
[cluster]
name = integration
node = node1
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[service app]
systemd_unit = app.service
home_node = node1
EOF

run_good_config_starts interface-display-name \
    "interface bond1.3675@bond1 normalized to bond1.3675" \
    "dry-run VIP del 127.0.0.201/32 on bond1.3675" <<EOF
[cluster]
name = integration
node = node1
bind = 127.0.0.1
port = $(node_port node1)
socket = $(node_socket node1)
metrics = false

[node node1]
role = full-member
address = 127.0.0.1

[vip vip1]
address = 127.0.0.201/32
interface = bond1.3675@bond1
EOF

log "group config validation regression passed"
