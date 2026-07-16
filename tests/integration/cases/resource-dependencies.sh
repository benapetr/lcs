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

[vip vip1]
address = 127.0.0.200/32
interface = lo

[vip vip2]
depends_on = vip1
address = 127.0.0.201/32
interface = lo

[vip vip3]
depends_on = vip2
address = 127.0.0.202/32
interface = lo
EOF
}

resource_active()
{
    local vip="$1"
    local addr="$2"
    status_text node1 2>/dev/null |
        grep -Fq "$vip $addr dev=lo state=active owner=node1"
}

resource_owner()
{
    local vip="$1"
    local addr="$2"
    local owner="$3"
    status_text node1 2>/dev/null |
        grep -Fq "$vip $addr dev=lo state=active owner=$owner"
}

resource_stopped()
{
    local vip="$1"
    local addr="$2"
    status_text node1 2>/dev/null |
        grep -Fq "$vip $addr dev=lo state=stopped owner=-"
}

trap cleanup_cluster EXIT

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1

wait_until 15 "vip1 active" resource_active vip1 127.0.0.200/32
wait_until 15 "vip2 active after vip1" resource_active vip2 127.0.0.201/32
wait_until 15 "vip3 active after vip2" resource_active vip3 127.0.0.202/32

log "stopping dependency vip1"
"$LCS" -s "$(node_socket node1)" resource stop vip1
wait_until 10 "vip3 stopped before dependency remains down" resource_stopped vip3 127.0.0.202/32
wait_until 10 "vip2 stopped before dependency remains down" resource_stopped vip2 127.0.0.201/32
wait_until 10 "vip1 stopped" resource_stopped vip1 127.0.0.200/32

sleep 1
resource_stopped vip2 127.0.0.201/32 || die "vip2 restarted while vip1 dependency was stopped"
resource_stopped vip3 127.0.0.202/32 || die "vip3 restarted while vip2 dependency was stopped"

log "starting dependency vip1"
"$LCS" -s "$(node_socket node2)" resource start vip1
wait_until 15 "vip1 active after start" resource_active vip1 127.0.0.200/32
wait_until 15 "vip2 active after dependency restart" resource_active vip2 127.0.0.201/32
wait_until 15 "vip3 active after dependency restart" resource_active vip3 127.0.0.202/32

log "moving dependency vip1 to node2"
"$LCS" -s "$(node_socket node1)" resource move vip1 node2
wait_until 15 "vip1 moved to node2" resource_owner vip1 127.0.0.200/32 node2
wait_until 15 "vip2 followed dependency to node2" resource_owner vip2 127.0.0.201/32 node2
wait_until 15 "vip3 followed dependency chain to node2" resource_owner vip3 127.0.0.202/32 node2

log "resource dependency regression passed"
