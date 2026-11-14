#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/lcs-install-state.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

make -C "$ROOT_DIR" install DESTDIR="$STAGE" >/dev/null
[[ "$(stat -c '%a' "$STAGE/var/lib/lcs")" == 700 ]]
[[ ! -e "$STAGE/etc/lcs/resources.json" ]]

DPKG_ROOT="$STAGE" sh "$ROOT_DIR/debian/lcs.postinst" configure
[[ -f "$STAGE/etc/lcs/lcs.conf" ]]
printf '%s\n' 'retained transaction state' >"$STAGE/var/lib/lcs/transaction.json"
printf '%s\n' 'retained resource definitions' >"$STAGE/etc/lcs/resources.json"
printf '%s\n' '# retained static config' >>"$STAGE/etc/lcs/lcs.conf"

# Simulate reinstall/upgrade: neither installation nor postinst may replace
# daemon-managed files or an administrator's existing static configuration.
make -C "$ROOT_DIR" install DESTDIR="$STAGE" >/dev/null
DPKG_ROOT="$STAGE" sh "$ROOT_DIR/debian/lcs.postinst" configure 1.1.1
[[ "$(stat -c '%a' "$STAGE/var/lib/lcs")" == 700 ]]
[[ "$(cat "$STAGE/var/lib/lcs/transaction.json")" == 'retained transaction state' ]]
[[ "$(cat "$STAGE/etc/lcs/resources.json")" == 'retained resource definitions' ]]
[[ "$(tail -1 "$STAGE/etc/lcs/lcs.conf")" == '# retained static config' ]]
echo "installation and Debian state-preservation tests passed"
