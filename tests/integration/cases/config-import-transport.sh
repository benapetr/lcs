#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
mkdir -p "$TEST_TMP"
python3 - "$LCS" "$TEST_TMP" <<'PY'
import json,os,socket,struct,subprocess,sys,threading,time
from pathlib import Path
cli,tmp=sys.argv[1:]; path=tmp+'/import-fake.sock'
source=Path(tmp)/'large.json'; source.write_bytes(b' '*524288)
def receive(sock,n):
    out=b''
    while len(out)<n:
        block=sock.recv(n-len(out))
        if not block: raise AssertionError('truncated BEGIN')
        out+=block
    return out
for mode in ('disconnect','old-daemon'):
    with socket.socket(socket.AF_UNIX) as server:
        if os.path.exists(path): os.unlink(path)
        server.bind(path); server.listen(1); errors=[]
        def serve():
            try:
                with server.accept()[0] as sock:
                    magic,kind,flags,count,seq=struct.unpack('!IHHII',receive(sock,16))
                    body=receive(sock,count)
                    assert kind==45 and struct.unpack('!HI',body[:6])==(1,524288)
                    if mode=='old-daemon':
                        error=struct.pack('!i',-1)+b'unsupported local CLI message'.ljust(128,b'\0')
                        sock.sendall(struct.pack('!IHHII',magic,5,0,len(error),seq)+error)
                    # Stop accepting bytes while the CLI is still streaming.
                    sock.shutdown(socket.SHUT_RD); time.sleep(.1)
            except Exception as e: errors.append(e)
        thread=threading.Thread(target=serve); thread.start()
        result=subprocess.run([cli,'-s',path,'--json','--request-id','import-transport','config','import',str(source)],capture_output=True,timeout=5)
        thread.join(5); assert not thread.is_alive() and not errors,errors
        assert result.returncode==3,(mode,result)
        response=json.loads(result.stdout)
        assert response['request_id']=='import-transport' and response['result']=='unknown' and response['committed_revision'] is None,response
PY
log 'import disconnect and unsupported-daemon transport retain structured unknown outcomes'
