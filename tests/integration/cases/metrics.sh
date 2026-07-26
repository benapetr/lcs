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
metrics = true
metrics_bind = 127.0.0.1
metrics_port = $metrics_port
vip_backend = netlink
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

[service app]
systemd_unit = app.service
EOF
}

fetch_metrics()
{
    local node="$1"
    local port="$((PORT_BASE + 100 + ${node#node}))"
    python3 - "$port" <<'PY'
import sys
import urllib.request

port = sys.argv[1]
with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=1) as response:
    sys.stdout.write(response.read().decode())
PY
}

metrics_has()
{
    local node="$1"
    local pattern="$2"
    fetch_metrics "$node" 2>/dev/null | grep -Fq "$pattern"
}

trap cleanup_cluster EXIT

prepare_cluster
start_node node1
wait_for_socket node1

wait_until 10 "vip resource metrics" \
    metrics_has node1 'lcs_resource_state{cluster="integration",resource="vip1",type="vip",state='
wait_until 10 "service resource metrics" \
    metrics_has node1 'lcs_resource_state{cluster="integration",resource="app",type="service",state='

metrics_out="$TEST_TMP/metrics.out"
fetch_metrics node1 >"$metrics_out"
grep -Fq 'lcs_resource_owner{cluster="integration",resource="app",type="service",node="node1"}' "$metrics_out" ||
    die "metrics missing service owner series"
if grep -Fq 'lcs_vip_' "$metrics_out"; then
    die "metrics still contains old lcs_vip_* series"
fi

log "metrics regression passed"
