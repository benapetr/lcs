#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LCSD="${LCSD:-$ROOT_DIR/lcsd}"
LCS="${LCS:-$ROOT_DIR/lcs}"
TEST_TMP="${TEST_TMP:-$(mktemp -d "${TMPDIR:-/tmp}/lcs-it.XXXXXX")}"
PORT_BASE="${PORT_BASE:-$((20000 + (RANDOM % 20000)))}"
LEASE_MS="${LEASE_MS:-1200}"
RENEW_MS="${RENEW_MS:-250}"
PEER_TIMEOUT_MS="${PEER_TIMEOUT_MS:-900}"

declare -a LCS_PIDS=()

log()
{
    printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2
}

die()
{
    log "ERROR: $*"
    dump_logs
    exit 1
}

cleanup_cluster()
{
    local pid
    for pid in "${LCS_PIDS[@]:-}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    sleep 0.2
    for pid in "${LCS_PIDS[@]:-}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
}

dump_logs()
{
    local file
    if [[ -d "$TEST_TMP/logs" ]]; then
        for file in "$TEST_TMP"/logs/*.log; do
            [[ -e "$file" ]] || continue
            printf '\n===== %s =====\n' "$file" >&2
            tail -n 120 "$file" >&2 || true
        done
    fi
}

require_binaries()
{
    [[ -x "$LCSD" ]] || die "missing lcsd binary at $LCSD; run make first"
    [[ -x "$LCS" ]] || die "missing lcs binary at $LCS; run make first"
}

node_port()
{
    case "$1" in
        node1) printf '%s\n' "$((PORT_BASE + 1))" ;;
        node2) printf '%s\n' "$((PORT_BASE + 2))" ;;
        node3) printf '%s\n' "$((PORT_BASE + 3))" ;;
        *) die "unknown node $1" ;;
    esac
}

node_socket()
{
    printf '%s/%s.sock\n' "$TEST_TMP" "$1"
}

node_config()
{
    printf '%s/%s.conf\n' "$TEST_TMP" "$1"
}

write_config()
{
    local self="$1"
    local metrics_port="$((PORT_BASE + 100 + ${self#node}))"
    cat >"$(node_config "$self")" <<EOF
[cluster]
name = integration
node = $self
bind = 127.0.0.1
port = $(node_port "$self")
socket = $(node_socket "$self")
syslog = false
metrics = false
metrics_bind = 127.0.0.1
metrics_port = $metrics_port
vip_backend = ip
lease_ms = $LEASE_MS
renew_ms = $RENEW_MS
peer_timeout_ms = $PEER_TIMEOUT_MS
probe_count = 1
probe_timeout_ms = 50
hook_timeout_ms = 1000

[node node1]
role = full-member
address = 127.0.0.1
port = $(node_port node1)

[node node2]
role = full-member
address = 127.0.0.1
port = $(node_port node2)

[node node3]
role = quorum-only
address = 127.0.0.1
port = $(node_port node3)

[vip vip1]
address = 127.0.0.200/32
interface = lo
EOF
}

prepare_cluster()
{
    require_binaries
    mkdir -p "$TEST_TMP/logs"
    write_config node1
    write_config node2
    write_config node3
}

start_node()
{
    local node="$1"
    log "starting $node"
    LCS_VIP_DRY_RUN=1 "$LCSD" -c "$(node_config "$node")" -f --no-syslog --no-timestamp -vv \
        >"$TEST_TMP/logs/$node.log" 2>&1 &
    LCS_PIDS+=("$!")
}

start_cluster()
{
    prepare_cluster
    start_node node1
    start_node node2
    start_node node3
}

stop_node()
{
    local node="$1"
    local idx="${node#node}"
    local arr_idx=$((idx - 1))
    local pid="${LCS_PIDS[$arr_idx]:-}"
    [[ -n "$pid" ]] || return 0
    log "stopping $node pid=$pid"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

status_text()
{
    "$LCS" -s "$(node_socket "$1")" status
}

wait_until()
{
    local timeout="$1"
    local desc="$2"
    shift 2
    local end=$((SECONDS + timeout))
    while (( SECONDS < end )); do
        if "$@"; then
            return 0
        fi
        sleep 0.1
    done
    die "timeout waiting for $desc"
}

node_status_has()
{
    local node="$1"
    local pattern="$2"
    status_text "$node" 2>/dev/null | grep -Fq "$pattern"
}

wait_for_socket()
{
    local node="$1"
    wait_until 8 "$node socket" test -S "$(node_socket "$node")"
}

wait_for_quorum()
{
    local node="$1"
    wait_until 12 "$node quorum" node_status_has "$node" "quorum: yes"
}

wait_for_owner()
{
    local node="$1"
    local owner="$2"
    wait_until 15 "vip1 owner $owner as seen by $node" \
        node_status_has "$node" "vip1 127.0.0.200/32 dev=lo state=active owner=$owner"
}

wait_for_node_offline()
{
    local observer="$1"
    local node="$2"
    wait_until 12 "$node offline as seen by $observer" \
        node_status_has "$observer" "$node role=full-member online=no"
}

assert_no_vip_ops_in_log()
{
    local node="$1"
    if grep -Eq 'VIP (add|del)|dry-run VIP' "$TEST_TMP/logs/$node.log"; then
        die "$node performed VIP manipulation"
    fi
}

