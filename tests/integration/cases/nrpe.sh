#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

run_nrpe()
{
    local node="$1"
    local expected_rc="$2"
    local expected_text="$3"
    local out="$TEST_TMP/nrpe-$node.out"
    set +e
    "$LCS" -s "$(node_socket "$node")" nrpe >"$out" 2>&1
    local rc=$?
    set -e
    [[ "$rc" -eq "$expected_rc" ]] || {
        cat "$out" >&2
        die "nrpe on $node returned $rc, expected $expected_rc"
    }
    grep -Fq "$expected_text" "$out" || {
        cat "$out" >&2
        die "nrpe on $node did not contain: $expected_text"
    }
}

write_critical_config()
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
role = quorum-only
address = 127.0.0.1
port = $(node_port node2)

[node node3]
role = quorum-only
address = 127.0.0.1
port = $(node_port node3)

[group service]
type = anti-affinity
mode = strict

[vip vip1]
group = service
priority = 2
address = 127.0.0.201/32
interface = lo

[vip vip2]
group = service
priority = 1
address = 127.0.0.202/32
interface = lo
EOF
}

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_owner node1 node1
run_nrpe node1 0 "OK - quorum=yes"

stop_node node1
wait_for_node_offline node2 node1
wait_for_owner node2 node2
run_nrpe node2 1 "WARNING - quorum=yes"

cleanup_cluster
LCS_PIDS=()
write_config()
{
    write_critical_config "$1"
}
start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_until 15 "vip1 active" node_status_has node1 "vip1 127.0.0.201/32 dev=lo state=active owner=node1"
wait_until 15 "vip2 stopped" node_status_has node1 "vip2 127.0.0.202/32 dev=lo state=stopped owner=-"
run_nrpe node1 2 "CRITICAL - quorum=yes"

log "nrpe regression passed"
