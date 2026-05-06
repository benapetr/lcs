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

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3

wait_until 15 "vip1 owner node1" vip_owner_has node1 vip1 node1
sleep 1

status_text node1 | grep -Fq "vip1 127.0.0.201/32 dev=lo state=active owner=node1" || die "vip1 not active on node1"
status_text node1 | grep -Fq "vip2 127.0.0.202/32 dev=lo state=stopped owner=-" || die "vip2 should remain stopped"
grep -Fq "strict anti-affinity group service has 2 VIPs but only 1 full-member nodes" "$TEST_TMP/logs/node1.log" ||
    die "missing strict anti-affinity warning"

log "group strict anti-affinity hold-down regression passed"
