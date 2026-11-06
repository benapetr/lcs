#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077
prepare_cluster
# Long valid UTF-8 hook paths expand in canonical ASCII JSON, forcing more than
# one frame without increasing the configured resource/path limits.
python3 - "$(node_config node1)" "$TEST_TMP" <<'PY'
from pathlib import Path
import shutil, sys
config, root = Path(sys.argv[1]), Path(sys.argv[2])
hook = root / ('é' * 100)
shutil.copy('/bin/true', hook)
hook.chmod(0o700)
assert len(str(hook).encode()) <= 255
text = config.read_text().split('[vip vip1]')[0]
for i in range(32):
    text += f'\n[vip vip{i:02}]\naddress = 127.0.0.{100+i}/32\ninterface = lo\n'
    for key in ('pre_start', 'post_start', 'pre_stop', 'post_stop'):
        text += f'{key} = {hook}\n'
config.write_text(text)
PY
start_node node1
wait_for_socket node1
"$LCS" -s "$(node_socket node1)" config export >"$TEST_TMP/export.json"
"$LCS" -s "$(node_socket node1)" --json config export applied >"$TEST_TMP/applied.json"
cmp "$TEST_TMP/export.json" "$TEST_TMP/applied.json" || die 'legacy committed/applied views differ'
python3 - "$TEST_TMP/export.json" <<'PY'
import json, sys
from pathlib import Path
data = Path(sys.argv[1]).read_bytes()
assert len(data) > 65536
value = json.loads(data)
assert value['schema_version'] == 1 and value['revision'] == 0 and len(value['resources']) == 32
assert value['resources']['vip00']['address'] == '127.0.0.100/32'
PY
# Malformed requests cannot initialize distributed history. Extra pipelined
# requests cannot append another response to a pinned one-request export stream.
python3 - "$(node_socket node1)" <<'PY'
import socket, struct, sys
def receive(s, length):
    data = b''
    while len(data) < length:
        part = s.recv(length-len(data))
        assert part
        data += part
    return data
for body in (b'', b'\0\2\1', b'\0\1\0', b'\0\1\1\0'):
    with socket.socket(socket.AF_UNIX) as s:
        s.connect(sys.argv[1]); s.sendall(struct.pack('!IHHII', 0x4c435331, 39, 0, len(body), 91)+body)
        magic, kind, flags, length, seq = struct.unpack('!IHHII', receive(s, 16))
        assert kind == 5 and seq == 91
        receive(s, length)
with socket.socket(socket.AF_UNIX) as s:
    s.settimeout(5); s.connect(sys.argv[1])
    request = struct.pack('!IHHII', 0x4c435331, 39, 0, 3, 92)+b'\0\1\1'
    s.sendall(request + request)
    received = 0
    while True:
        _, kind, _, length, seq = struct.unpack('!IHHII', receive(s, 16))
        assert kind == 40 and seq == 92
        payload = receive(s, length)
        version, authority, selection, revision, total, offset = struct.unpack('!HBBQII', payload[:20])
        assert (version, authority, selection, revision, offset) == (1, 0, 1, 0, received)
        received += len(payload)-20
        if received == total: break
    assert not s.recv(1)
PY
for file in activation.json agreement.json application.json barrier.json; do
    [[ ! -e "$(node_config node1).state/$file" ]] || die 'export initialized distributed state'
done
if "$LCS" -s "$(node_socket node1)" config export pending >"$TEST_TMP/bad.json" 2>/dev/null; then
    die 'invalid export selection succeeded'
fi
[[ ! -s "$TEST_TMP/bad.json" ]] || die 'invalid selection emitted partial output'
python3 - "$LCS" "$TEST_TMP/fake.sock" <<'PY'
import os, socket, struct, subprocess, sys, threading
cli, path = sys.argv[1:]
def frame(seq, body, kind=40):
    return struct.pack('!IHHII', 0x4c435331, kind, 0, len(body), seq) + body
def chunk(offset=0, total=2, revision=1, selection=1, authority=1, data=b'{'):
    return struct.pack('!HBBQII', 1, authority, selection, revision, total, offset) + data
for mode in ('short', 'gap', 'overlap', 'revision', 'authority', 'selection', 'sequence', 'oversized', 'old-daemon'):
    if os.path.exists(path): os.unlink(path)
    with socket.socket(socket.AF_UNIX) as server:
        server.bind(path); server.listen(1)
        errors = []
        def serve():
            try:
                with server.accept()[0] as client:
                    request = b''
                    while len(request) < 19: request += client.recv(19-len(request))
                    seq = struct.unpack('!IHHII', request[:16])[-1]
                    if mode == 'oversized': client.sendall(frame(seq, chunk(total=524289))); return
                    if mode == 'old-daemon':
                        message = b'unsupported request'
                        client.sendall(frame(seq, struct.pack('!i', -1) + message.ljust(128, b'\0'), kind=5)); return
                    client.sendall(frame(seq, chunk()))
                    if mode == 'short': return
                    second = chunk(offset=1, data=b'}')
                    if mode == 'gap': second = chunk(offset=2, total=3, data=b'}')
                    if mode == 'overlap': second = chunk(offset=0, data=b'}')
                    if mode == 'revision': second = chunk(offset=1, revision=2, data=b'}')
                    if mode == 'authority': second = chunk(offset=1, authority=0, revision=0, data=b'}')
                    if mode == 'selection': second = chunk(offset=1, selection=2, data=b'}')
                    client.sendall(frame(seq + (mode == 'sequence'), second))
            except Exception as error: errors.append(error)
        thread = threading.Thread(target=serve); thread.start()
        result = subprocess.run([cli, '-s', path, 'config', 'export'], capture_output=True, timeout=5)
        thread.join(5)
        assert not thread.is_alive() and not errors, errors
        assert result.returncode == 1 and not result.stdout, (mode, result)
        if mode == 'old-daemon': assert b'unsupported request' in result.stderr
PY
log 'large read-only INI export and strict request regression passed'
