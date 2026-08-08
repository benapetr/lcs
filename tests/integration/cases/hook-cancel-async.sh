#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

prepare_cluster
hook_script="$TEST_TMP/slow-post-start.sh"
hook_child_file="$TEST_TMP/hook-child.pid"
cat >"$hook_script" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
sleep 30 &
child=$!
printf '%s\n' "$child" >"$LCS_HOOK_CHILD_FILE"
wait "$child"
EOF
chmod +x "$hook_script"
printf 'post_start = %s\n' "$hook_script" >>"$(node_config node1)"
export LCS_EXTRA_ENV_node1="LCS_HOOK_CHILD_FILE=$hook_child_file"

start_node node1
start_node node2
start_node node3
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_owner node1 node1
wait_until 5 "post-start hook descendant" test -s "$hook_child_file"

hook_child_pid="$(sed -n '1p' "$hook_child_file")"
log "moving VIP while its post-start hook is still running"
"$LCS" -s "$(node_socket node1)" resource move vip1 node2 >/dev/null
timeout 1 "$LCS" -s "$(node_socket node1)" status >/dev/null ||
    die "node1 CLI blocked while cancelling a hook"
wait_for_owner node1 node2
wait_until 5 "hook process-group termination" \
    bash -c '! kill -0 "$1" 2>/dev/null' _ "$hook_child_pid"
grep -Fq "cancelling post-start hook for resource vip1" \
    "$TEST_TMP/logs/node1.log" ||
    die "post-start hook cancellation was not logged"

log "asynchronous hook cancellation regression passed"
