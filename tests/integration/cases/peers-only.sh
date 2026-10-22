#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT
prepare_cluster
for node in node1 node2 node3; do
    config="$(node_config "$node")"
    sed -i '/^\[cluster\]$/a peers_only = true' "$config"
    sed -i 's|allow_from = 127.0.0.0/8|allow_from = 0.0.0.0/0|' "$config"
    sed -i '/^\[node node2\]$/,/^$/ s/address = 127.0.0.1/address = 127.0.0.2/' "$config"
done

start_node node3
wait_for_socket node3

python3 - "$LCSD" "$LCS" "$(node_config node1)" "$(node_socket node3)" \
    "$(node_port node2)" "$(node_port node3)" "$TEST_TMP/logs/node1.log" <<'PY' || die "peers-only HELLO identity check failed"
import json
import os
import socket
import struct
import subprocess
import sys
import time

daemon, cli, config, receiver_socket, capture_port, receiver_port, log_path = sys.argv[1:]


def receive_frame(sock):
    def exact(size):
        data = b""
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            assert chunk, "connection closed before frame completed"
            data += chunk
        return data
    header = exact(16)
    magic, kind, flags, length, seq = struct.unpack("!IHHII", header)
    assert magic == 0x4c435331 and flags == 0 and length <= 65536
    payload = exact(length)
    return kind, header + payload, payload


def node1_connected():
    view = json.loads(subprocess.check_output([cli, "-s", receiver_socket, "--json", "status"]))
    return view["nodes"][0]["state"] != "offline"


# Capture a real HELLO from node1 to the placeholder node2. Reusing that
# frame avoids duplicating the HELLO format or fingerprint algorithm here.
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.2", int(capture_port)))
    listener.listen(4)
    listener.settimeout(8)
    with open(log_path, "w") as log:
        sender = subprocess.Popen([daemon, "-c", config, "--no-syslog", "--no-timestamp", "-vv"],
                                  stdout=log, stderr=subprocess.STDOUT,
                                  env={**os.environ, "LCS_VIP_DRY_RUN": "1", "LCS_SYSTEMD_DRY_RUN": "1"})
        try:
            captured, _ = listener.accept()
            with captured:
                captured.settimeout(3)
                kind, hello, _ = receive_frame(captured)
                assert kind == 16

            deadline = time.monotonic() + 8
            while not node1_connected():
                assert time.monotonic() < deadline, "valid node1 connection was rejected"
                time.sleep(0.05)

            # Also check that identity rejection precedes processing a claimed
            # configuration-transition proof. In the released v5 HELLO prefix,
            # the voting fingerprint begins at byte 24 and commit_proof at 43.
            transition_hello = bytearray(hello)
            transition_hello[16 + 24] ^= 1
            transition_hello[16 + 43] = 1
            for frame in (hello, transition_hello):
                # This source is configured as node2, so it passes the initial
                # IP gate, but it must not be able to claim node1's identity.
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as wrong_source:
                    wrong_source.settimeout(3)
                    wrong_source.bind(("127.0.0.2", 0))
                    wrong_source.connect(("127.0.0.1", int(receiver_port)))
                    wrong_source.sendall(frame)
                    kind, _, payload = receive_frame(wrong_source)
                    assert kind == 5, "mismatched source received HELLO_ACK"
                    assert b"source IP does not match the claimed node" in payload
                assert node1_connected(), "rejected HELLO displaced the legitimate session"
        finally:
            sender.terminate()
            try:
                sender.wait(timeout=5)
            except subprocess.TimeoutExpired:
                sender.kill()
                sender.wait()
PY

log "peers-only HELLO source binding regression passed"
