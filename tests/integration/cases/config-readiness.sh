#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
prepare_cluster
python3 - "$LCSD" "$LCS" "$(node_config node1)" "$(node_socket node1)" \
    "$(node_port node2)" "$TEST_TMP/logs/node1.log" <<'PY' || die "explicit readiness protocol regression failed"
import json, os, socket, struct, subprocess, sys, time
from pathlib import Path

daemon, cli, config, cli_socket, port, log_path = sys.argv[1:]
MAGIC = 0x4c435331
INSTANCE = 0x123456789ABCDEF


def frame(sock):
    def exact(size):
        data = b""
        while len(data) < size:
            part = sock.recv(size - len(data))
            if not part:
                raise EOFError("connection closed")
            data += part
        return data
    magic, kind, flags, length, seq = struct.unpack("!IHHII", exact(16))
    assert magic == MAGIC and flags == 0 and length <= 65536
    return kind, seq, exact(length)


def send(sock, kind, seq, payload):
    sock.sendall(struct.pack("!IHHII", MAGIC, kind, 0, len(payload), seq) + payload)


def receive_type(sock, wanted):
    for _ in range(64):
        kind, seq, payload = frame(sock)
        if kind == wanted:
            return payload
    raise AssertionError("expected frame not received")


def establish(listener, version):
    sock, _ = listener.accept()
    sock.settimeout(4)
    kind, seq, hello = frame(sock)
    assert kind == 16
    # Reuse the daemon's real fingerprints and released HELLO layout. Only
    # configured member identity, negotiated version and instance are changed.
    ack = bytearray(hello)
    struct.pack_into("!HHH", ack, 0, 5, version, 1)
    struct.pack_into("!Q", ack, 16, INSTANCE)
    ack[60:124] = b"node2" + b"\0" * 59
    send(sock, 17, seq, ack)
    return sock, struct.unpack_from("!Q", hello, 16)[0]


def closed(sock):
    try:
        while True:
            kind, _, _ = frame(sock)
            assert kind not in (32, 33), "rejected request received a configuration reply"
    except (EOFError, ConnectionResetError):
        return


with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", int(port)))
    listener.listen(4)
    listener.settimeout(8)
    with open(log_path, "w") as log:
        process = subprocess.Popen([daemon, "-c", config, "--no-syslog", "--no-timestamp", "-vv"],
            stdout=log, stderr=subprocess.STDOUT,
            env={**os.environ, "LCS_VIP_DRY_RUN": "1", "LCS_SYSTEMD_DRY_RUN": "1"})
        try:
            sock, local_instance = establish(listener, 6)
            with sock:
                status = receive_type(sock, 30)
                assert len(status) == 141 and status[:2] == b"\0\1"
                digests = status[11:]
                def query(attempt, requester=INSTANCE, values=digests):
                    return struct.pack("!HBQQQB", 1, 1, requester, attempt, 0, 0) + values
                for attempt in (1, 2):
                    send(sock, 31, 60000 + attempt, query(attempt))
                    reply = receive_type(sock, 32)
                    assert len(reply) == 158
                    assert struct.unpack("!HBQQQB", reply[:28]) == (1, 2, INSTANCE, attempt, local_instance, 2)
                    assert reply[28:] == digests
                changed = digests[:65] + b"0" * 64 + b"\0"
                send(sock, 31, 60003, query(3, values=changed))
                reply = receive_type(sock, 32)
                assert reply[27] == 3 and reply[28:] == changed
                # A valid but unsolicited READY cannot count as local authority.
                unsolicited = struct.pack("!HBQQQB", 1, 2, local_instance, 999, INSTANCE, 1) + digests
                send(sock, 32, 60004, unsolicited)
                send(sock, 31, 60005, query(5))
                assert receive_type(sock, 32)[27] == 2
                # Transaction requests are separate and cannot initialize an INI-only node.
                membership, baseline = digests[:65], digests[65:]
                def prepare(txid, ballot=33):
                    return struct.pack("!HBBQQQQQQQ", 1, 1, 0, INSTANCE, local_instance,
                                       txid, 1, ballot, 0, 0) + membership + baseline + b"\0" * 65
                send(sock, 33, 61000, prepare(42))
                promise = receive_type(sock, 33)
                assert len(promise) == 255 and promise[2:4] == bytes((2, 3))
                assert struct.unpack("!QQQ", promise[4:28]) == (local_instance, INSTANCE, 42)
                metadata = (struct.pack("!BBHQQQQII", 1, 1, 0, 43, 1, 33, 0, 0, 16) +
                            membership + baseline + b"1" * 64 + b"\0" + b"upgrade" + b"\0" * 57)
                send(sock, 34, 61001, struct.pack("!HQQ", 1, INSTANCE, local_instance) + metadata)
                rejected = receive_type(sock, 33)
                assert rejected[2:4] == bytes((3, 3))
                # A revision-zero checkpoint query is also nonmutating in legacy mode.
                sync_query = (struct.pack("!HBBQQQQQQQ", 1, 5, 0, INSTANCE, local_instance,
                                          45, 0, 0, 0, 0) + membership + baseline + baseline)
                send(sock, 33, 61003, sync_query)
                status = receive_type(sock, 33)
                assert len(status) == 255 and status[2:4] == bytes((6, 3))
                send(sock, 31, 60006, query(6, requester=INSTANCE + 1))
                closed(sock)
            sock, _ = establish(listener, 5)
            with sock:
                send(sock, 31, 60007, query(7))
                closed(sock)
            sock, local_instance = establish(listener, 5)
            with sock:
                send(sock, 33, 61002, prepare(44))
                closed(sock)
            state = Path(config + ".state")
            for name in ("activation.json", "barrier.json", "agreement.json", "application.json"):
                assert not (state / name).exists(), f"readiness query created {name}"
            view = json.loads(subprocess.check_output([cli, "-s", cli_socket, "--json", "config", "status"]))
            assert view["authority"] == "ini" and view["dynamic_commands"] is False
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
PY
log "explicit readiness framing, admission, and legacy nonmutation regression passed"
