#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
prepare_cluster
python3 - "$LCSD" "$LCS" "$(node_config node1)" "$(node_config node3)" \
    "$(node_port node2)" "$(node_port node3)" "$TEST_TMP/logs" <<'PY' || die 'freeze exchange regression failed'
import contextlib, json, os, re, socket, struct, subprocess, sys, time
from pathlib import Path

daemon, cli, config1, config3, port2, port3, logs = sys.argv[1:]
MAGIC = 0x4c435331
seq = 60000
for config in (config1, config3):
    path = Path(config)
    path.write_text(re.sub(r'(?m)^peer_timeout_ms\s*=\s*\d+\s*$', 'peer_timeout_ms = 10000', path.read_text()))

def frame(sock):
    def exact(size):
        data = b''
        while len(data) < size:
            part = sock.recv(size-len(data))
            if not part: raise EOFError()
            data += part
        return data
    magic, kind, flags, length, sequence = struct.unpack('!IHHII', exact(16))
    assert magic == MAGIC and not flags and length <= 65536
    return kind, sequence, exact(length)

def send(sock, kind, payload, sequence=None):
    global seq
    seq += 1
    sock.sendall(struct.pack('!IHHII', MAGIC, kind, 0, len(payload), sequence or seq) + payload)

def receive(sock, wanted):
    for _ in range(128):
        kind, _, payload = frame(sock)
        if kind == wanted: return payload
    raise AssertionError('expected frame absent')

def identity(template, node, version, instance):
    body = bytearray(template)
    struct.pack_into('!HHH', body, 0, 5, version, node)
    struct.pack_into('!H', body, 12, 2 if node == 2 else 1)
    struct.pack_into('!Q', body, 16, instance)
    name = f'node{node+1}'.encode()
    body[60:124] = name + b'\0' * (64-len(name))
    return body

def accept(listener, node, version, instance):
    sock, _ = listener.accept(); sock.settimeout(5)
    kind, sequence, hello = frame(sock); assert kind == 16
    send(sock, 17, identity(hello, node, version, instance), sequence)
    return sock, struct.unpack_from('!Q', hello, 16)[0], hello

def closed(sock):
    try:
        while True:
            kind, _, _ = frame(sock)
            assert kind != 48, 'rejected frame received freeze reply'
    except (EOFError, ConnectionResetError): pass

@contextlib.contextmanager
def running(config, node):
    with open(Path(logs)/f'node{node}.log', 'w') as log:
        p = subprocess.Popen([daemon, '-c', config, '--no-syslog', '--no-timestamp', '-vv'],
            stdout=log, stderr=subprocess.STDOUT,
            env={**os.environ, 'LCS_VIP_DRY_RUN':'1', 'LCS_SYSTEMD_DRY_RUN':'1'})
        try: yield p
        finally:
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()

def query(sock, requester, responder, attempt, digests):
    send(sock, 47, struct.pack('!HBQQQB', 1, 1, requester, attempt, responder, 0) + digests)

def reply_for(sock, requester, responder, attempt):
    for _ in range(128):
        reply = receive(sock, 48)
        assert len(reply) == 158
        if struct.unpack('!HBQQQB', reply[:28])[:5] == (1, 2, requester, attempt, responder): return reply
    raise AssertionError('correlated freeze reply absent')

def assert_no_history(config):
    for name in ('agreement', 'application', 'requests', 'barrier'):
        assert not Path(config+'.state', name+'.json').exists(), name

def frozen(sock, config, requester, responder, attempt, digests):
    query(sock, requester, responder, attempt, digests)
    for _ in range(32):
        reply = reply_for(sock, requester, responder, attempt)
        fields = struct.unpack('!HBQQQB', reply[:28])
        assert fields[:5] == (1, 2, requester, attempt, responder) and reply[28:] == digests
        assert fields[5] in (1, 2)
        if fields[5] == 1: break
    else: raise AssertionError('durable freeze acknowledgement absent')
    record = Path(config+'.state', 'activation.json')
    contents = record.read_bytes(); value = json.loads(contents)
    assert value['record_version'] == 3 and value['phase'] == 'frozen'
    assert_no_history(config)
    # Exact retry retains the record and cannot promote or thaw it.
    query(sock, requester, responder, attempt, digests)
    assert reply_for(sock, requester, responder, attempt)[27] == 1 and record.read_bytes() == contents
    return contents

with socket.socket() as l2, socket.socket() as l3:
    for listener, port in ((l2, port2), (l3, port3)):
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', int(port))); listener.listen(4); listener.settimeout(8)
    with running(config1, 1):
        s2, local, template = accept(l2, 1, 6, 200)
        s3, _, _ = accept(l3, 2, 5, 300)
        digests = receive(s2, 30)[11:]
        query(s2, 200, local, 1, digests)
        assert reply_for(s2, 200, local, 1)[27] == 2 # Old quorum-only peer prevents freeze.
        assert not Path(config1+'.state', 'activation.json').exists()
        # Wrong destination process and wire-5 sessions reject freeze traffic.
        query(s2, 200, local+1, 2, digests); closed(s2); s2.close()
        s2, local, _ = accept(l2, 1, 5, 200)
        query(s2, 200, local, 3, digests); closed(s2); s2.close()
        s2, local, _ = accept(l2, 1, 6, 200)
        s3.close(); s3, _, _ = accept(l3, 2, 6, 300)
        changed = digests[:65] + (b'0' if digests[65:66] != b'0' else b'1') + digests[66:]
        query(s2, 200, local, 4, changed)
        assert reply_for(s2, 200, local, 4)[27] == 3
        assert not Path(config1+'.state', 'activation.json').exists()
        contents1 = frozen(s2, config1, 200, local, 5, digests)
        s2.close(); s3.close()

# A quorum-only responder uses identical persistence and acknowledgements.
with running(config3, 3):
    sockets = []
    try:
        for node, instance in ((0, 100), (1, 200)):
            until = time.monotonic()+8
            while True:
                try: sock = socket.create_connection(('127.0.0.1', int(port3)), timeout=5); break
                except ConnectionRefusedError:
                    if time.monotonic() >= until: raise
                    time.sleep(.05)
            sock.settimeout(5); sockets.append(sock)
            send(sock, 16, identity(template, node, 6, instance))
            ack = receive(sock, 17); local3 = struct.unpack_from('!Q', ack, 16)[0]
        contents3 = frozen(sockets[0], config3, 100, local3, 1, digests)
    finally:
        for sock in sockets: sock.close()

for config, contents in ((config1, contents1), (config3, contents3)):
    node = 1 if config == config1 else 3
    with running(config, node):
        until = time.monotonic()+8
        cli_socket = str(Path(config).with_suffix('.sock'))
        while True:
            result = subprocess.run([cli, '-s', cli_socket, '--json', 'config', 'status'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if result.returncode == 0: break
            assert time.monotonic() < until
            time.sleep(.05)
        status = json.loads(result.stdout)
        assert status['application_state'] == 'frozen' and not status['admitted'] and not status['dynamic_commands']
        if node == 3:
            # Recovered freezes admit coordination only over wire 6; a fresh
            # correlated query can read the retained fact without another write.
            with socket.create_connection(('127.0.0.1', int(port3)), timeout=5) as sock:
                sock.settimeout(5); send(sock, 16, identity(template, 0, 5, 100)); closed(sock)
            with socket.create_connection(('127.0.0.1', int(port3)), timeout=5) as sock:
                sock.settimeout(5); send(sock, 16, identity(template, 0, 6, 100))
                ack = receive(sock, 17); resumed_instance = struct.unpack_from('!Q', ack, 16)[0]
                assert frozen(sock, config, 100, resumed_instance, 2, digests) == contents
        assert Path(config+'.state', 'activation.json').read_bytes() == contents
        assert_no_history(config)
PY
assert_no_vip_ops_in_log node3
log 'wire-6 explicit freeze exchange, instance/version admission, full/quorum-only durable responses and restart retention passed'
