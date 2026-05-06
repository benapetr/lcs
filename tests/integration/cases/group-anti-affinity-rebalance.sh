#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

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

[group service]
type = anti-affinity
mode = best-effort

[vip vip1]
group = service
priority = 1
address = 127.0.0.201/32
interface = lo

[vip vip2]
group = service
priority = 2
address = 127.0.0.202/32
interface = lo
EOF
}

vip_owner_has()
{
    local node="$1"
    local vip="$2"
    local owner="$3"
    status_text "$node" 2>/dev/null | grep -F "$vip " | grep -Fq "owner=$owner"
}

trap cleanup_cluster EXIT

prepare_cluster
start_node node1
start_node node3
wait_for_socket node1
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node3

wait_until 15 "vip1 owner node1 before node2 joins" vip_owner_has node1 vip1 node1
wait_until 15 "vip2 owner node1 before node2 joins" vip_owner_has node1 vip2 node1

start_node node2
wait_for_socket node2
wait_for_quorum node1
wait_for_quorum node2

wait_until 20 "vip2 rebalanced to node2" vip_owner_has node1 vip2 node2
status_text node1 | grep -Fq "vip1 127.0.0.201/32 dev=lo state=active owner=node1" || die "vip1 not active on node1 after rebalance"
status_text node1 | grep -Fq "vip2 127.0.0.202/32 dev=lo state=active owner=node2" || die "vip2 not rebalanced to node2"

log "group anti-affinity rebalance regression passed"
