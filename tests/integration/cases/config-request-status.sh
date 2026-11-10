#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
prepare_cluster
start_node node1
wait_for_socket node1
"$LCS" -s "$(node_socket node1)" --json config request-status missing | python3 -c '
import json, sys
s = json.load(sys.stdin)
assert s == dict(scope="local", request_id="missing", history="unavailable", state="unknown",
                 base_revision=None, value_revision=None, base_digest=None, value_digest=None)
'
"$LCS" -s "$(node_socket node1)" config request-status missing >"$TEST_TMP/status.txt"
grep -Fq 'state: unknown' "$TEST_TMP/status.txt" || die 'missing text request state'
for id in '' 'bad/id' "$(printf '%064d' 1)"; do
    result=0
    "$LCS" -s "$TEST_TMP/nonexistent.sock" --json config request-status "$id" >"$TEST_TMP/invalid.json" 2>/dev/null || result=$?
    [[ "$result" == 2 && ! -s "$TEST_TMP/invalid.json" ]] || die 'invalid ID did not fail locally with usage status'
done
python3 - "$(node_socket node1)" "$LCS" "$TEST_TMP/fake.sock" <<'PY'
import json, os, socket, struct, subprocess, sys, threading
path, cli, fake = sys.argv[1:]
def receive(s, length):
    data = b''
    while len(data) < length:
        part = s.recv(length-len(data)); assert part
        data += part
    return data
def frame(seq, body, kind=42):
    return struct.pack('!IHHII', 0x4c435331, kind, 0, len(body), seq) + body
request = b'\0\1' + b'missing'.ljust(64, b'\0')
for bad in (b'', request[:-1], request+b'\0', b'\0\2'+request[2:], request[:-1]+b'x', b'\0\1'+b'a'*64):
    with socket.socket(socket.AF_UNIX) as s:
        s.connect(path); s.sendall(frame(91, bad, 41))
        _, kind, _, length, seq = struct.unpack('!IHHII', receive(s, 16))
        assert kind == 5 and seq == 91
        receive(s, length)
for mode in ('unknown', 'reserved', 'committed', 'applied', 'recovery', 'short', 'sequence', 'id', 'version', 'state', 'history', 'padding', 'unknown-data', 'old-daemon'):
    if os.path.exists(fake): os.unlink(fake)
    errors = []
    with socket.socket(socket.AF_UNIX) as server:
        server.bind(fake); server.listen(1); server.settimeout(5)
        def serve():
            try:
                with server.accept()[0] as s:
                    header = receive(s, 16)
                    _, kind, _, size, seq = struct.unpack('!IHHII', header)
                    assert kind == 41 and receive(s, size) == request
                    if mode == 'old-daemon':
                        s.sendall(frame(seq, struct.pack('!i', -1)+b'unsupported request'.ljust(128, b'\0'), 5)); return
                    state = 1 if mode == 'reserved' else 2 if mode == 'committed' else 3 if mode == 'applied' else 0
                    history = 2 if mode == 'recovery' else 1
                    identity = b'other' if mode == 'id' else b'missing'
                    body = struct.pack('!HBB64sQ65s65s', 2 if mode == 'applied' else 1, history, state, identity, 7 if state else 0,
                                       b'a'*64 if state else b'', b'b'*64 if state else b'')
                    if mode == 'short': body = body[:-1]
                    if mode == 'version': body = b'\0\3' + body[2:]
                    if mode == 'state': body = body[:3] + b'\x04' + body[4:]
                    if mode == 'history': body = body[:2] + b'\x03' + body[3:]
                    if mode == 'padding': body = body[:67] + b'x' + body[68:]
                    if mode == 'unknown-data': body = body[:76] + b'a' + body[77:]
                    s.sendall(frame(seq + (mode == 'sequence'), body))
            except Exception as error: errors.append(error)
        thread = threading.Thread(target=serve); thread.start()
        result = subprocess.run([cli, '-s', fake, '--json', 'config', 'request-status', 'missing'], capture_output=True, timeout=5)
        thread.join(5); assert not thread.is_alive() and not errors, errors
        if mode in ('unknown', 'reserved', 'committed', 'applied', 'recovery'):
            assert result.returncode == 0, (mode, result)
            view = json.loads(result.stdout)
            assert view['scope'] == 'local' and view['state'] == ('unknown' if mode == 'recovery' else mode)
            assert view['history'] == ('recovery-required' if mode == 'recovery' else 'available')
            assert view['value_revision'] == (8 if mode in ('reserved', 'committed', 'applied') else None)
        else:
            assert result.returncode == 1 and not result.stdout, (mode, result)
            if mode == 'old-daemon': assert b'unsupported request' in result.stderr
PY
for file in requests.json agreement.json application.json activation.json barrier.json; do
    [[ ! -e "$(node_config node1).state/$file" ]] || die 'request query initialized distributed history'
done
log 'local read-only request-status and response validation regression passed'
