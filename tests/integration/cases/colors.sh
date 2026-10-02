#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT

check_colors()
{
    python3 - "$LCS" "$(node_socket node1)" "$1" <<'PY'
import errno
import json
import os
import pty
import re
import subprocess
import sys

binary, socket, phase = sys.argv[1:]
env = dict(os.environ, TERM="xterm")
env.pop("NO_COLOR", None)
ansi = re.compile(rb"\x1b\[[0-9;]*m")

def normalize(output):
    # Membership age can tick between consecutive status requests.
    return re.sub(rb"membership(?: for |_for=)[0-9dhms ]+",
                  b"membership age", output)

def run(command, terminal=True, extra_env=None):
    args = [binary, "-s", socket, *command]
    process_env = dict(env, **(extra_env or {}))
    if not terminal:
        return subprocess.run(args, env=process_env, stdout=subprocess.PIPE,
                              check=False).stdout
    master, slave = pty.openpty()
    try:
        process = subprocess.Popen(args, env=process_env, stdout=slave)
        os.close(slave)
        slave = -1
        output = b""
        while True:
            try:
                chunk = os.read(master, 4096)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not chunk:
                break
            output += chunk
        process.wait(timeout=10)
        return output.replace(b"\r\n", b"\n")
    finally:
        os.close(master)
        if slave >= 0:
            os.close(slave)

for command in (["status"], ["resource", "list"]):
    colored = run(command)
    assert ansi.search(colored), colored
    plain = ansi.sub(b"", colored)
    for actual in (run(["--no-colors", *command]),
                   run(command, terminal=False),
                   run(command, extra_env={"NO_COLOR": "1"}),
                   run(command, extra_env={"TERM": "dumb"})):
        assert not ansi.search(actual), actual
        assert normalize(actual) == normalize(plain), (actual, plain)
    json_output = run(["--json", *command])
    assert not ansi.search(json_output), json_output
    json.loads(json_output)

nrpe = run(["nrpe"])
assert not ansi.search(nrpe), nrpe
assert normalize(nrpe) == normalize(run(["nrpe"], terminal=False))
json.loads(run(["--json", "nrpe"]))

status = run(["status"])
if phase == "healthy":
    assert b"quorum: \x1b[32myes\x1b[0m" in status, status
    assert b"state=\x1b[32monline\x1b[0m" in status, status
    assert b"state=\x1b[32mactive\x1b[0m" in status, status
    assert nrpe.startswith(b"OK - "), nrpe
elif phase == "offline":
    assert b"state=\x1b[31moffline\x1b[0m" in status, status
    assert nrpe.startswith(b"WARNING - "), nrpe
elif phase == "disabled":
    assert b"state=\x1b[33mstopped\x1b[0m" in status, status
    assert b"\x1b[33mdisabled=yes\x1b[0m" in status, status
PY
}

start_cluster
wait_for_socket node1
wait_for_socket node2
wait_for_socket node3
wait_for_quorum node1
wait_for_quorum node2
wait_for_quorum node3
wait_for_owner node1 node1
check_colors healthy

stop_node node3
wait_until 12 "quorum-only node offline" node_status_has node1 \
    "node3 role=quorum-only state=offline"
check_colors offline

"$LCS" -s "$(node_socket node1)" resource stop vip1
disabled_stopped()
{
    status_text node1 | grep -F "state=stopped" | grep -Fq "disabled=yes"
}
wait_until 8 "administratively stopped resource" disabled_stopped
check_colors disabled
log "terminal colors regression passed"
