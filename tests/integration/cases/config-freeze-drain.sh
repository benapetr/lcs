#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
prepare_cluster
python3 - "$LCSD" "$LCS" "$(node_config node1)" "$(node_port node2)" "$(node_port node3)" "$TEST_TMP" <<'PY' || die 'warm freeze drain failed'
import json,os,re,select,socket,struct,subprocess,sys,time
from pathlib import Path

daemon,cli,template,port2,port3,tmp=sys.argv[1:]
MAGIC=0x4c435331
sequence=60000

def send(sock,kind,payload,seq=None):
    global sequence
    sequence+=1
    sock.sendall(struct.pack('!IHHII',MAGIC,kind,0,len(payload),sequence if seq is None else seq)+payload)

def receive(sock):
    def exact(n):
        out=b''
        while len(out)<n:
            part=sock.recv(n-len(out))
            if not part: raise EOFError()
            out+=part
        return out
    magic,kind,flags,n,seq=struct.unpack('!IHHII',exact(16))
    assert magic==MAGIC and not flags and n<=65536
    return kind,seq,exact(n)

def identity(body,node,instance):
    b=bytearray(body)
    struct.pack_into('!HHH',b,0,5,6,node)
    struct.pack_into('!H',b,12,2 if node==2 else 1)
    struct.pack_into('!Q',b,16,instance)
    name=f'node{node+1}'.encode(); b[60:124]=name.ljust(64,b'\0')
    return b

for failed in (False,True):
    config=template+('-failure' if failed else '-success')
    sockpath=config+'.sock'; trace=Path(config+'.trace'); hook=Path(config+'.hook'); marker=Path(config+'.fail')
    hook.write_text('#!/bin/sh\nprintf "%s|%s\\n" "$LCS_EVENT" "$LCS_RESOURCE" >> '+str(trace)+'\n')
    hook.chmod(0o700)
    text=Path(template).read_text()
    text=re.sub(r'(?m)^socket\s*=.*$', 'socket = '+sockpath,text)
    text=re.sub(r'(?m)^peer_timeout_ms\s*=.*$', 'peer_timeout_ms = 2000',text)
    Path(config).write_text(text+f'\npre_stop = {hook}\npost_stop = {hook}\n')
    listeners=[]; peers={}; replies=[]; digests=None; next_heartbeat=0; confirm_locks=False
    log=open(Path(tmp)/('warm-failure.log' if failed else 'warm-success.log'),'w')
    try:
        for port in (port2,port3):
            s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            s.bind(('127.0.0.1',int(port))); s.listen(2); s.settimeout(8); listeners.append(s)
        p=subprocess.Popen([daemon,'-c',config,'--no-syslog','--no-timestamp','-vv'],stdout=log,stderr=subprocess.STDOUT,
            env={**os.environ,'LCS_VIP_DRY_RUN':'1','LCS_SYSTEMD_DRY_RUN':'1','LCS_VIP_FAIL_DEL_FILE':str(marker)})
        for node,instance,listener in ((1,200,listeners[0]),(2,300,listeners[1])):
            s,_=listener.accept(); s.settimeout(3); kind,seq,hello=receive(s); assert kind==16
            local=struct.unpack_from('!Q',hello,16)[0]
            send(s,17,identity(hello,node,instance),seq); peers[s]=instance
            # A joining peer sends its own stopped generation as well as ACK.
            state=struct.pack('!QH',instance,1)+struct.pack('!HHQB',0,65535,0,0)+bytes(5*8+1+8+1+128)
            send(s,18,state)
        def pump(seconds):
            global next_heartbeat,digests
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                if time.monotonic()>=next_heartbeat:
                    for s in peers: send(s,26,b'\1')
                    next_heartbeat=time.monotonic()+.2
                ready,_,_=select.select(list(peers),[],[],.02)
                for s in ready:
                    kind,seq,body=receive(s)
                    if kind==18:
                        # Same definitions/state, distinct admitted peer instance.
                        send(s,19,struct.pack('!Q',peers[s])+body[8:],seq)
                    elif kind in (20,22,23): send(s,21,struct.pack('!I',0)+b'ok'.ljust(128,b'\0'),seq)
                    elif kind==30:
                        digests=body[11:]
                        # Keep one peer metadata-unknown so this test controls
                        # freeze initiation explicitly after acquiring the VIP.
                        if peers[s]==200: send(s,30,body)
                    elif kind==47 and confirm_locks:
                        reply=bytearray(body); reply[2]=2; reply[27]=4; send(s,48,reply)
                    elif kind==48: replies.append(body)
        def status():
            r=subprocess.run([cli,'-s',sockpath,'--json','status'],capture_output=True,timeout=3)
            return json.loads(r.stdout) if r.returncode==0 else None
        until=time.monotonic()+12
        while True:
            pump(.1); current=status()
            if current and current['resources'][0]['state']=='active' and current['resources'][0]['owner']=='node1': break
            assert time.monotonic()<until, 'VIP never became locally active'
        epoch=current['resources'][0]['epoch']; assert digests and len(digests)==130
        if failed: marker.touch()
        source=next(s for s in peers if peers[s]==200)
        send(source,47,struct.pack('!HBQQQB',1,1,200,1,local,0)+digests)
        # A durable local lock must keep the old owner renewing until every
        # member confirms its own lock, even across multiple lease periods.
        lease_ms=int(re.search(r'(?m)^lease_ms\s*=\s*(\d+)',text).group(1))
        until=time.monotonic()+2*lease_ms/1000+.3
        while time.monotonic()<until:
            pump(.1)
            current=status()
            assert current and current['resources'][0]['state']=='active'
            assert current['resources'][0]['epoch']==epoch
            assert not trace.exists(), 'owner drained before all durable locks'
        assert Path(config+'.state/activation.json').exists()
        assert any(r[27]==4 for r in replies), 'durable lock was not acknowledged'
        confirm_locks=True
        # The original responder waiter may have expired; use a fresh attempt.
        send(source,47,struct.pack('!HBQQQB',1,1,200,2,local,0)+digests)
        until=time.monotonic()+8
        while True:
            pump(.1)
            matching=[r for r in replies if struct.unpack('!HBQQQB',r[:28])[:5]==(1,2,200,2,local)]
            if failed:
                r=subprocess.run([cli,'-s',sockpath,'--json','config','status'],capture_output=True,timeout=3)
                if r.returncode==0 and 'cleanup failed' in json.loads(r.stdout)['error']: break
            elif any(r[27]==1 for r in matching): break
            assert time.monotonic()<until, 'freeze drain never reached its expected outcome'
        current=status(); assert current and not current['cluster']['quorum']
        assert current['resources'][0]['epoch']>=epoch
        record=Path(config+'.state/activation.json'); assert json.loads(record.read_text())['phase']=='frozen'
        for name in ('agreement','application','requests','barrier'): assert not Path(config+'.state',name+'.json').exists()
        if failed:
            assert not any(r[27]==1 for r in replies), 'failed cleanup acknowledged a freeze'
            assert current['resources'][0]['state']=='stop_failed'
            retained=record.read_bytes()
            # Repair the backend and restart: recover the retained freeze, never
            # reset it to INI or reconstruct initialization without every receipt.
            marker.unlink(); p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
            for s in peers: s.close()
            peers.clear(); replies.clear(); next_heartbeat=0
            # Discard reconnect attempts queued by the old process during shutdown.
            for listener in listeners:
                listener.settimeout(.02)
                while True:
                    try: stale,_=listener.accept(); stale.close()
                    except socket.timeout: break
                listener.settimeout(8)
            p=subprocess.Popen([daemon,'-c',config,'--no-syslog','--no-timestamp','-vv'],stdout=log,stderr=subprocess.STDOUT,
                env={**os.environ,'LCS_VIP_DRY_RUN':'1','LCS_SYSTEMD_DRY_RUN':'1'})
            for node,instance,listener in ((1,200,listeners[0]),(2,300,listeners[1])):
                s,_=listener.accept(); s.settimeout(3); kind,seq,hello=receive(s); assert kind==16
                local=struct.unpack_from('!Q',hello,16)[0]; send(s,17,identity(hello,node,instance),seq); peers[s]=instance
            source=next(s for s in peers if peers[s]==200)
            send(source,47,struct.pack('!HBQQQB',1,1,200,3,local,0)+digests)
            until=time.monotonic()+8
            while not any(r[27]==1 for r in replies):
                pump(.1); assert time.monotonic()<until, 'repaired frozen restart failed to drain'
            assert record.read_bytes()==retained
            for name in ('agreement','application','requests','barrier'): assert not Path(config+'.state',name+'.json').exists()
        else:
            assert current['resources'][0]['state']=='stopped' and current['resources'][0]['owner'] is None
            events=trace.read_text().splitlines(); assert events==['pre-stop|vip1','post-stop|vip1'], events
    finally:
        for s in peers: s.close()
        for s in listeners: s.close()
        if 'p' in locals():
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        log.close()
PY
log 'warm freeze drains owned VIPs with old hooks and fences failed cleanup without acknowledging initialization'
