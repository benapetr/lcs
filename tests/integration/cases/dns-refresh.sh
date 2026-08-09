#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

write_config()
{
    local self="$1"
    cat >"$(node_config "$self")" <<EOF
[cluster]
name = integration
node = $self
bind = 127.0.0.1
port = $(node_port "$self")
socket = $(node_socket "$self")
syslog = false
metrics = false
lease_ms = $LEASE_MS
renew_ms = $RENEW_MS
peer_timeout_ms = $PEER_TIMEOUT_MS

[node node1]
role = full-member
address = localhost
port = $(node_port node1)

[node node2]
role = quorum-only
address = localhost
port = $(node_port node2)

[node node3]
role = quorum-only
address = localhost
port = $(node_port node3)
EOF
}

trap cleanup_cluster EXIT

# Startup resolution remains real and must succeed.  Only node1's asynchronous
# refresh worker injects a deterministic failure for node2.
LCS_EXTRA_ENV_node1="LCS_DNS_REFRESH_TEST_FAILURE=node2"
start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1

log "refreshing peer DNS cache through lcs reload"
"$LCS" -s "$(node_socket node1)" reload >/dev/null
wait_until 8 "failed DNS refresh retained node2 cache" \
    grep -Fq "DNS refresh for peer node2 address localhost failed" \
    "$TEST_TMP/logs/node1.log"
wait_until 8 "successful unchanged DNS refresh for node3" \
    grep -Fq "DNS cache refresh for peer node3 address localhost is unchanged" \
    "$TEST_TMP/logs/node1.log"

node_status_has node1 "node2 role=quorum-only state=online" ||
    die "failed DNS refresh disconnected node2"
node_status_has node1 "quorum: yes" ||
    die "failed DNS refresh disturbed quorum"
if grep -Fq "peer node2 offline" "$TEST_TMP/logs/node1.log"; then
    die "DNS refresh failure discarded the active node2 connection"
fi

log "safe DNS cache refresh regression passed"
