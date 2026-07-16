#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

json_get()
{
    local expr="$1"
    python3 -c '
import json
import sys

data = json.load(sys.stdin)
value = eval(sys.argv[1], {"__builtins__": {}}, {"data": data})
if isinstance(value, bool):
    print("true" if value else "false")
elif value is None:
    print("null")
else:
    print(value)
' "$expr"
}

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_owner node1 node1

status_json="$("$LCS" --json -s "$(node_socket node1)" status)"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["cluster"]["quorum"]')" == "true" ]] ||
    die "status JSON did not report quorum"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["resources"][0]["name"]')" == "vip1" ]] ||
    die "status JSON missing vip1"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["resources"][0]["owner"]')" == "node1" ]] ||
    die "status JSON owner mismatch"

resource_json="$("$LCS" -s "$(node_socket node1)" --json resource list)"
[[ "$(printf '%s\n' "$resource_json" | json_get 'data["resources"][0]["state"]')" == "active" ]] ||
    die "resource list JSON did not report active vip1"

nrpe_json="$("$LCS" -s "$(node_socket node1)" --json nrpe)"
[[ "$(printf '%s\n' "$nrpe_json" | json_get 'data["state"]')" == "OK" ]] ||
    die "nrpe JSON did not report OK"

log "json output regression passed"
