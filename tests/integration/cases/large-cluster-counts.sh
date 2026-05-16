#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

write_large_config()
{
    local self="$1"
    local count="$2"
    local metrics_port="$((PORT_BASE + 100 + ${self#node}))"
    local i

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

EOF

    for ((i = 1; i <= count; i++)); do
        cat >>"$(node_config "$self")" <<EOF
[node node$i]
role = full-member
address = 127.0.0.1
port = $(node_port "node$i")

EOF
    done

    cat >>"$(node_config "$self")" <<EOF
[vip vip1]
address = 127.0.0.200/32
interface = lo
EOF
}

prepare_large_cluster()
{
    local count="$1"
    local i
    require_binaries
    mkdir -p "$TEST_TMP/logs"
    for ((i = 1; i <= count; i++)); do
        write_large_config "node$i" "$count"
    done
}

start_large_cluster()
{
    local count="$1"
    local i
    prepare_large_cluster "$count"
    for ((i = 1; i <= count; i++)); do
        start_node "node$i"
    done
}

wait_for_large_sockets()
{
    local count="$1"
    local i
    for ((i = 1; i <= count; i++)); do
        wait_for_socket "node$i"
    done
}

wait_for_votes()
{
    local node="$1"
    local votes="$2"
    local need="$3"
    wait_until 20 "$node votes $votes need $need" \
        node_status_has "$node" "quorum: yes ($votes votes, need $need,"
}

run_large_cluster_case()
{
    local count="$1"
    local need=$((count / 2 + 1))
    local failover_owner="node2"
    local tmp_root="$TEST_TMP"

    if (( count >= 10 )); then
        failover_owner="node10"
    fi

    TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/lcs-it-${count}.XXXXXX")"
    PORT_BASE="$((20000 + (RANDOM % 15000) + count * 100))"
    LCS_PIDS=()

    log "large cluster scenario: $count nodes need=$need"
    start_large_cluster "$count"
    wait_for_large_sockets "$count"
    wait_for_votes node1 "$count" "$need"
    wait_for_votes "node$count" "$count" "$need"
    wait_for_owner node2 node1

    stop_node node1
    wait_for_node_offline node2 node1
    wait_for_votes node2 "$((count - 1))" "$need"
    wait_for_owner node2 "$failover_owner"
    wait_for_owner "node$count" "$failover_owner"

    cleanup_cluster
    TEST_TMP="$tmp_root"
}

trap cleanup_cluster EXIT

run_large_cluster_case 6
run_large_cluster_case 7
run_large_cluster_case 10

log "large cluster count regressions passed"
