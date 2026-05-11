#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

HOOK_LOG="$TEST_TMP/hooks.log"
HOOK_SCRIPT="$TEST_TMP/hook-record.sh"

write_hook_script()
{
    cat >"$HOOK_SCRIPT" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s node=%s vip=%s event=%s epoch=%s lease=%s address=%s interface=%s\n' \
    "$(date +%s)" "$LCS_NODE" "$LCS_VIP" "$LCS_EVENT" "$LCS_EPOCH" \
    "$LCS_LEASE_ID" "$LCS_ADDRESS" "$LCS_INTERFACE" >>"$LCS_HOOK_LOG"
EOF
    chmod +x "$HOOK_SCRIPT"
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
pre_start = $HOOK_SCRIPT
post_start = $HOOK_SCRIPT
post_stop = $HOOK_SCRIPT
EOF
}

start_node()
{
    local node="$1"
    log "starting $node"
    LCS_VIP_DRY_RUN=1 LCS_HOOK_LOG="$HOOK_LOG" \
        "$LCSD" -c "$(node_config "$node")" --no-syslog --no-timestamp -vv \
        >"$TEST_TMP/logs/$node.log" 2>&1 &
    LCS_PIDS+=("$!")
}

hook_has()
{
    local pattern="$1"
    [[ "$HOOK_LOG" ]] && grep -Fq "$pattern" "$HOOK_LOG"
}

log_has()
{
    local node="$1"
    local pattern="$2"
    grep -Fq "$pattern" "$TEST_TMP/logs/$node.log"
}

trap cleanup_cluster EXIT

write_hook_script
start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3
wait_for_owner node1 node1

wait_until 10 "node1 pre-start hook" hook_has "node=node1 vip=vip1 event=pre-start"
wait_until 10 "node1 post-start hook" hook_has "node=node1 vip=vip1 event=post-start"

log "moving vip1 from node1 to node2 through node1 CLI"
"$LCS" -s "$(node_socket node1)" move vip1 node2

wait_for_owner node1 node2
wait_for_owner node2 node2
wait_until 10 "node1 post-stop hook" hook_has "node=node1 vip=vip1 event=post-stop"
wait_until 10 "node2 post-start hook" hook_has "node=node2 vip=vip1 event=post-start"

wait_until 10 "node1 pre-start hook start log" log_has node1 "started pre-start hook for VIP vip1"
wait_until 10 "node1 post-start hook completion log" log_has node1 "post-start hook for VIP vip1 completed status=ok"
wait_until 10 "node1 post-stop hook start log" log_has node1 "started post-stop hook for VIP vip1"
wait_until 10 "node1 post-stop hook completion log" log_has node1 "post-stop hook for VIP vip1 completed status=ok"

log "hook execution regression passed"
