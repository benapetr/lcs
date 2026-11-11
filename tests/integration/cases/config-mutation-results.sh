#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
prepare_cluster
start_node node1
wait_for_socket node1
result=0
"$LCS" -s "$(node_socket node1)" --json --request-id legacy-edit resource create extra vip address=127.0.0.203/32 interface=lo >"$TEST_TMP/legacy.json" || result=$?
(( result == 1 )) || die 'INI cluster did not reject definition edits'
python3 - "$TEST_TMP/legacy.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='rejected' and 'activated JSON' in s['reason'],s
PY
[[ ! -e "$(node_config node1).state/requests.json" ]] || die 'legacy mutation created request history'
python3 - "$LCS" "$TEST_TMP" <<'PY'
import json,os,socket,struct,subprocess,sys,threading
cli,tmp=sys.argv[1:]; path=tmp+'/fake-mutation.sock'
def receive(s,n):
    out=b''
    while len(out)<n:
        chunk=s.recv(n-len(out))
        if not chunk: raise AssertionError('truncated request')
        out+=chunk
    return out
for mode in ('applied','rejected','pending','unknown','disconnect','old-daemon','sequence','id','format','truncated','padding','false-applied'):
    with socket.socket(socket.AF_UNIX) as server:
        if os.path.exists(path): os.unlink(path)
        server.bind(path); server.listen(1); errors=[]
        def serve():
            try:
                with server.accept()[0] as s:
                    magic,kind,flags,n,seq=struct.unpack('!IHHII',receive(s,16)); body=receive(s,n)
                    assert magic==0x4c435331 and kind==43 and flags==0
                    assert body[2:66].rstrip(b'\0')==b'client-edit'
                    if mode=='disconnect': return
                    result={'applied':0,'rejected':1,'pending':2}.get(mode,3)
                    state=3 if mode=='applied' else 2 if mode=='pending' else 0
                    identity=b'foreign-edit' if mode=='id' else b'client-edit'
                    view=struct.pack('!HBB64sQ65s65s',2 if state==3 else 1,1,state,identity,7 if state else 0,b'a'*64 if state else b'',b'b'*64 if state else b'')
                    reply=struct.pack('!HB',1,result)+view+b'local outcome'.ljust(256,b'\0')
                    if mode=='format': reply=b'\0\2'+reply[2:]
                    if mode=='padding': reply=reply[:-1]+b'x'
                    if mode=='false-applied': reply=reply[:2]+b'\0'+reply[3:]
                    if mode=='truncated': reply=reply[:-1]
                    reply_type=5 if mode=='old-daemon' else 44
                    s.sendall(struct.pack('!IHHII',magic,reply_type,0,len(reply),seq+(mode=='sequence'))+reply)
            except Exception as e: errors.append(e)
        thread=threading.Thread(target=serve); thread.start()
        result=subprocess.run([cli,'-s',path,'--json','--request-id','client-edit','resource','update','vip1','priority=66'],capture_output=True,timeout=5)
        thread.join(5); assert not thread.is_alive() and not errors,errors
        expected={'applied':0,'rejected':1,'pending':2}.get(mode,3)
        assert result.returncode==expected,(mode,result)
        output=json.loads(result.stdout)
        assert output['request_id']=='client-edit' and output['result']==('unknown' if expected==3 else mode),(mode,output)
        if expected==3: assert output['applied_revision'] is None,(mode,output)
PY
log 'legacy refusal, mutation JSON/exit results and uncertain transport responses passed'
