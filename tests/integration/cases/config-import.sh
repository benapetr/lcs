#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077
prepare_cluster
read -r -a fixture_flags <<<"$(pkg-config --cflags --libs jansson libcrypto)"
"${CC:-cc}" -D_GNU_SOURCE -I"$ROOT_DIR/src" -std=c11 -Wall -Wextra -Wpedantic \
    "$SCRIPT_DIR/../helpers/config_recovery_fixture.c" \
    "$ROOT_DIR/src/"{config_requests,config_activation,config_barrier,config_agreement,config_application,config_store,config_candidate,resource_config,config,util,source_allowlist}.c \
    "${fixture_flags[@]}" -o "$TEST_TMP/fixture"
for node in node1 node2 node3; do
    "$TEST_TMP/fixture" "$(node_config "$node")"
    "$TEST_TMP/fixture" "$(node_config "$node")" history
done
start_node node1
start_node node2
start_node node3
admitted_revision() {
    "$LCS" -s "$(node_socket "$1")" --json config status 2>/dev/null | python3 -c '
import json,sys
s=json.load(sys.stdin); assert s["admitted"] and s["committed_revision"]==s["applied_revision"]==int(sys.argv[1])
' "$2" 2>/dev/null
}
for node in node1 node2 node3; do wait_until 40 "$node import startup" admitted_revision "$node" 3; done
"$LCS" -s "$(node_socket node1)" config export >"$TEST_TMP/before.json"
python3 - "$TEST_TMP" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]); s=json.loads((p/'before.json').read_text())
s['revision']=999; s['resources']['vip1']['birth_revision']=900
s['resources']['vip1']['priority']=42; s['resources']['vip1']['group']='vips'
s['resources']['vip2']={'type':'vip','address':'127.0.0.204/32','interface':'lo','group':'vips','birth_revision':900}
s['groups']={'vips':{'type':'anti-affinity','mode':'best-effort'}}
text=json.dumps(s).encode(); assert len(text)<65536
(p/'bulk.json').write_bytes(text+b' '*(524288-len(text)))
(p/'same-content.json').write_bytes((p/'bulk.json').read_bytes())
(p/'changed-bytes.json').write_bytes(text)
s['resources']['vip2'].pop('interface'); (p/'invalid.json').write_text(json.dumps(s))
(p/'empty.json').write_text(json.dumps({'schema_version':1,'cluster':s['cluster'],'revision':0,'groups':{},'resources':{}}))
(p/'oversized.json').write_bytes(b' ' * 524289)
PY
# Partial, malformed, interleaved, duplicated, oversized and expired uploads cannot reserve anything.
python3 - "$LCS" "$(node_socket node1)" <<'PY'
import json,socket,struct,subprocess,sys,time
cli,path=sys.argv[1:]
def frame(kind,body,seq=123): return struct.pack('!IHHII',0x4c435331,kind,0,len(body),seq)+body
ids=[]
for mode in ('partial','duplicate','offset','sequence','interleaved','overrun','total','format','expired'):
    identity=('upload-'+mode).encode(); ids.append(identity.decode())
    args=[b'config',b'import',b'json']
    descriptor=struct.pack('!H64sBQ65sH',1,identity,0,0,b'',3)+b''.join(struct.pack('!H',len(a))+a for a in args)
    begin=struct.pack('!HI',2 if mode=='format' else 1,524289 if mode=='total' else 20)+descriptor
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(12); sock.connect(path); sock.sendall(frame(45,begin))
        if mode=='expired':
            time.sleep(5); sock.sendall(frame(46,struct.pack('!HI',1,0)+b'x'))
            # A chunk does not extend the original ten-second upload deadline.
            started=time.monotonic(); assert sock.recv(1)==b''; assert time.monotonic()-started<7
        elif mode not in ('total','format'):
            sock.sendall(frame(46,struct.pack('!HI',1,1 if mode=='offset' else 0)+(b'x'*21 if mode=='overrun' else b'x'),124 if mode=='sequence' else 123))
            if mode=='duplicate': sock.sendall(frame(46,struct.pack('!HI',1,0)+b'x'))
            if mode=='interleaved': sock.sendall(frame(14,b''))
            time.sleep(.05)
    s=json.loads(subprocess.check_output([cli,'-s',path,'--json','config','request-status',identity.decode()]))
    assert s['state']=='unknown',(mode,s)
PY
import_value() {
    local id="$1" revision="$2" input="$3"; shift 3
    local result attempt
    for attempt in {1..20}; do
        result=0
        if [[ "$input" == - ]]; then
            "$LCS" -s "$(node_socket node1)" --json --request-id "$id" "$@" config import - <"$TEST_TMP/empty.json" >"$TEST_TMP/result.json" || result=$?
        else
            "$LCS" -s "$(node_socket node1)" --json --request-id "$id" "$@" config import "$input" >"$TEST_TMP/result.json" || result=$?
        fi
        (( result != 1 )) && break
        grep -Eq 'busy|not admitted' "$TEST_TMP/result.json" || die "import rejected: $(cat "$TEST_TMP/result.json")"
        sleep .2
    done
    (( result != 1 )) || die 'import remained busy'
    for node in node1 node2 node3; do wait_until 40 "$node bulk revision $revision" admitted_revision "$node" "$revision"; done
}
import_value bulk-add 4 "$TEST_TMP/bulk.json" --expected-revision 3
for node in node1 node2 node3; do
    "$LCS" -s "$(node_socket "$node")" config export >"$TEST_TMP/$node-export.json"
done
python3 - "$TEST_TMP" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]); values=[json.loads((p/(n+'-export.json')).read_text()) for n in ('node1','node2','node3')]
assert values[0]==values[1]==values[2]; s=values[0]
assert s['revision']==4 and len(s['resources'])==2 and s['groups']['vips']['type']=='anti-affinity'
assert s['resources']['vip1']['birth_revision']==3 and s['resources']['vip2']['birth_revision']==4
assert s['resources']['vip1']['priority']==42
PY
for input in invalid.json changed-bytes.json; do
    result=0; id=invalid-import
    [[ "$input" != changed-bytes.json ]] || id=bulk-add
    for attempt in {1..20}; do
        result=0
        "$LCS" -s "$(node_socket node1)" --json --request-id "$id" config import "$TEST_TMP/$input" >"$TEST_TMP/rejected.json" || result=$?
        (( result == 1 )) || die 'invalid/conflicting import was not rejected'
        grep -Eq 'busy|not admitted' "$TEST_TMP/rejected.json" || break
        sleep .2
    done
done
"$LCS" -s "$(node_socket node1)" config export >"$TEST_TMP/after-invalid.json"
cmp "$TEST_TMP/node1-export.json" "$TEST_TMP/after-invalid.json" || die 'invalid import changed definitions'
result=0
"$LCS" -s "$(node_socket node1)" --json --request-id oversized config import "$TEST_TMP/oversized.json" >"$TEST_TMP/input-error.json" || result=$?
(( result == 1 )) || die 'oversized input accepted'
import_value clear-all 5 -
for node in node1 node2 node3; do
    "$LCS" -s "$(node_socket "$node")" config export | python3 -c 'import json,sys; s=json.load(sys.stdin); assert s["revision"]==5 and not s["resources"] and not s["groups"]'
done
# Exact bytes, different local filename and a newer current base are one known retry.
"$LCS" -s "$(node_socket node1)" --json --request-id bulk-add config import "$TEST_TMP/same-content.json" >"$TEST_TMP/old-retry.json"
stop_node node1
start_node node1
LCS_PIDS[0]="${LCS_PIDS[-1]}"
wait_until 40 'empty-catalog origin restart' admitted_revision node1 5
"$LCS" -s "$(node_socket node1)" --json --request-id bulk-add config import "$TEST_TMP/bulk.json" >"$TEST_TMP/restarted-retry.json"
python3 - "$TEST_TMP" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1])
for f in ('old-retry.json','restarted-retry.json'):
 s=json.loads((p/f).read_text()); assert s['result']=='applied' and s['applied_revision']==4,s
h=json.loads((p/'node1.conf.state/requests.json').read_text())
assert all(not r['request_id'].startswith('upload-') and r['request_id'] not in ('invalid-import','oversized') for r in h['requests'])
PY
assert_no_vip_ops_in_log node3
log 'atomic streamed import, source metadata normalization, rejection, stdin and restart retry passed'
