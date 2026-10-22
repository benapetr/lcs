#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"

trap cleanup_cluster EXIT
prepare_cluster

python3 - "$LCSD" "$LCS" "$(node_config node3)" "$TEST_TMP" <<'PY' || die "source IP allowlist checks failed"
import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

daemon, cli, template, root = sys.argv[1:]
base = Path(template).read_text()


def exercise(name, policy, bind, source, allowed, peers_only=False, peer_address="127.0.0.2"):
    config = re.sub(r"^allow_from\s*=.*\n", "", base, flags=re.M)
    config = config.replace("[cluster]\n", f"[cluster]\npeers_only = {'true' if peers_only else 'false'}\n", 1)
    config = re.sub(r"(\[node node2\]\nrole = full-member\naddress = ).*",
                    lambda match: match[1] + peer_address, config)
    if policy is not None:
        config = config.replace("[cluster]\n", f"[cluster]\nallow_from = {policy}\n", 1)
    config = re.sub(r"^bind\s*=.*$", f"bind = {bind}", config, flags=re.M)
    sock_path = str(Path(root) / f"{name}.sock")
    config = re.sub(r"^socket\s*=.*$", f"socket = {sock_path}", config, flags=re.M)
    port = int(re.search(r"^port\s*=\s*(\d+)", config, re.M)[1])
    path = Path(root) / f"{name}.conf"
    path.write_text(config)
    log_path = Path(root) / "logs" / f"{name}.log"
    with log_path.open("w") as log:
        proc = subprocess.Popen([daemon, "-c", str(path), "--no-syslog", "--no-timestamp", "-v"],
                                stdout=log, stderr=subprocess.STDOUT,
                                env={**os.environ, "LCS_VIP_DRY_RUN": "1"})
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                assert proc.poll() is None, f"{name}: daemon exited"
                if Path(sock_path).exists():
                    result = subprocess.run([cli, "-s", sock_path, "status"], capture_output=True)
                    if result.returncode == 0:
                        break
                time.sleep(0.05)
            else:
                raise AssertionError(f"{name}: daemon did not become ready")

            family = socket.AF_INET6 if ":" in bind else socket.AF_INET
            with socket.socket(family, socket.SOCK_STREAM) as client:
                client.settimeout(2)
                client.bind((source, 0))
                client.connect((bind, port))
                received = b""
                try:
                    # An empty HELLO is syntactically framed but invalid. Allowed
                    # sources reach the HELLO decoder and receive LCS_MSG_ERROR;
                    # denied sources are closed before any protocol response.
                    client.sendall(struct.pack("!IHHII", 0x4c435331, 16, 0, 0, 1))
                    while len(received) < 16:
                        chunk = client.recv(16 - len(received))
                        if not chunk:
                            break
                        received += chunk
                except (ConnectionResetError, BrokenPipeError):
                    pass
                if allowed:
                    assert len(received) == 16, f"{name}: allowed source was rejected"
                    magic, kind, flags, length, seq = struct.unpack("!IHHII", received)
                    assert (magic, kind, flags, seq) == (0x4c435331, 5, 0, 1)
                else:
                    assert not received, f"{name}: denied source reached HELLO"
                    deadline = time.monotonic() + 2
                    while "not allowed by allow_from" not in log_path.read_text():
                        assert time.monotonic() < deadline, f"{name}: missing rejection diagnostic"
                        time.sleep(0.02)
                    assert "invalid HELLO payload" not in log_path.read_text()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    print(f"{name}: passed", flush=True)


exercise("default-denies-loopback", None, "127.0.0.1", "127.0.0.1", False)
exercise("explicit-host-allowed", "127.0.0.1/32", "127.0.0.1", "127.0.0.1", True)
exercise("other-host-denied", "127.0.0.1/32", "127.0.0.1", "127.0.0.2", False)
exercise("ipv4-bypass", "0.0.0.0/0", "127.0.0.1", "127.0.0.2", True)
exercise("peers-only-disabled", "0.0.0.0/0", "127.0.0.1", "127.0.0.3", True)
exercise("configured-peer-allowed", "0.0.0.0/0", "127.0.0.1", "127.0.0.2", True, peers_only=True)
exercise("unconfigured-source-denied", "0.0.0.0/0", "127.0.0.1", "127.0.0.3", False, peers_only=True)
exercise("configured-peer-subnet-denied", "127.0.0.1/32", "127.0.0.1", "127.0.0.2", False, peers_only=True)
try:
    with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as probe:
        probe.bind(("::1", 0))
except OSError:
    print("IPv6 loopback unavailable; IPv6 socket checks skipped", flush=True)
else:
    exercise("default-denies-ipv6", None, "::1", "::1", False)
    exercise("explicit-ipv6-allowed", "::1/128", "::1", "::1", True)
    exercise("ipv6-bypass", "0.0.0.0/0", "::1", "::1", True)
    exercise("configured-ipv6-peer-allowed", "::1/128", "::1", "::1", True,
             peers_only=True, peer_address="::1")
    exercise("unconfigured-ipv6-source-denied", "0.0.0.0/0", "::1", "::1", False, peers_only=True)
PY

log "source IP allowlist regression passed"
