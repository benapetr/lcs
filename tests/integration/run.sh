#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<EOF
usage: tests/integration/run.sh [case ...]

Runs opt-in localhost integration tests. These tests use LCS_VIP_DRY_RUN=1,
temporary configs, temporary Unix sockets, and random localhost TCP ports.

Available cases:
EOF
    for case_file in "$ROOT_DIR"/tests/integration/cases/*.sh; do
        printf '  %s\n' "$(basename "$case_file" .sh)"
    done
    exit 0
fi

if [[ ! -x "$ROOT_DIR/lcsd" || ! -x "$ROOT_DIR/lcs" ]]; then
    make -C "$ROOT_DIR"
fi

cases=("$@")
if [[ ${#cases[@]} -eq 0 ]]; then
    cases=()
    for case_file in "$ROOT_DIR"/tests/integration/cases/*.sh; do
        cases+=("$(basename "$case_file" .sh)")
    done
fi

for case_name in "${cases[@]}"; do
    case_path="$ROOT_DIR/tests/integration/cases/$case_name.sh"
    [[ -x "$case_path" ]] || {
        echo "missing or non-executable test case: $case_name" >&2
        exit 1
    }
    echo "==> $case_name"
    "$case_path"
done

echo "all integration tests passed"
