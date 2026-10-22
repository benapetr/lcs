#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT

expected_version="$(sed -n 's/^#define LCS_VERSION "\([^"]*\)"/\1/p' "$ROOT_DIR/src/common.h")"
[[ -n "$expected_version" ]] || die "could not read release version from src/common.h"

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
[[ "$(printf '%s\n' "$status_json" | json_get 'data["cluster"]["effective_protocol"]')" == "5" ]] ||
    die "status JSON did not report effective protocol"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["cluster"]["effective_protocol_release"]')" == "1.1.0" ]] ||
    die "status JSON did not map effective protocol to release"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["nodes"][0]["state"]')" == "online" ]] ||
    die "status JSON did not report node state"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["resources"][0]["name"]')" == "vip1" ]] ||
    die "status JSON missing vip1"
[[ "$(printf '%s\n' "$status_json" | json_get 'data["resources"][0]["owner"]')" == "node1" ]] ||
    die "status JSON owner mismatch"

status_text_output="$(status_text node1)"
printf '%s\n' "$status_text_output" | grep -Fq "protocol: 1.1.0 (wire 5)" ||
    die "text status did not report mapped effective protocol"

resource_json="$("$LCS" -s "$(node_socket node1)" --json resource list)"
[[ "$(printf '%s\n' "$resource_json" | json_get 'data["resources"][0]["state"]')" == "active" ]] ||
    die "resource list JSON did not report active vip1"

nrpe_json="$("$LCS" -s "$(node_socket node1)" --json nrpe)"
[[ "$(printf '%s\n' "$nrpe_json" | json_get 'data["state"]')" == "OK" ]] ||
    die "nrpe JSON did not report OK"

reload_json="$("$LCS" -s "$(node_socket node1)" --json reload)"
[[ "$(printf '%s\n' "$reload_json" | json_get 'data["ok"]')" == "true" ]] ||
    die "reload JSON did not report an accepted request"

version_json_a="$("$LCS" --json --version)"
version_json_b="$("$LCS" --version --json)"
[[ "$(printf '%s\n' "$version_json_a" | json_get 'data["build_flags"]["WITH_SYSTEMD"]')" == "$(printf '%s\n' "$version_json_b" | json_get 'data["build_flags"]["WITH_SYSTEMD"]')" ]] ||
    die "version JSON changed with option order"
for version_json in "$version_json_a" "$version_json_b"; do
    [[ "$(printf '%s\n' "$version_json" | json_get 'data["version"]')" == "$expected_version" ]] ||
        die "version JSON did not report release version $expected_version"
done

log "json output regression passed"
