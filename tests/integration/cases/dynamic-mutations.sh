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
s=json.load(sys.stdin)
assert s["admitted"] and s["applied_revision"] == s["committed_revision"] == int(sys.argv[1])
' "$2" 2>/dev/null
}
for node in node1 node2 node3; do wait_until 40 "$node initial admission" admitted_revision "$node" 3; done
mutation() {
    local id="$1" revision="$2"; shift 2
    local attempt result
    for attempt in {1..20}; do
        result=0
        "$LCS" -s "$(node_socket node1)" --json --request-id "$id" "$@" >"$TEST_TMP/result.json" || result=$?
        if (( result != 1 )); then break; fi
        grep -Eq 'busy|not admitted' "$TEST_TMP/result.json" || die "mutation $id rejected: $(cat "$TEST_TMP/result.json")"
        sleep 0.2
    done
    (( result != 1 )) || die "mutation $id remained busy"
    python3 - "$TEST_TMP/result.json" "$id" "$result" <<'PYTHON'
import json,sys
s=json.load(open(sys.argv[1]))
assert s['request_id']==sys.argv[2] and s['result']=={0:'applied',2:'pending',3:'unknown'}[int(sys.argv[3])],s
PYTHON
    for node in node1 node2 node3; do wait_until 40 "$node mutation $id admission" admitted_revision "$node" "$revision"; done
    "$LCS" -s "$(node_socket node1)" --json --request-id "$id" "$@" >"$TEST_TMP/retry.json"
    python3 - "$TEST_TMP/retry.json" "$revision" <<'PYTHON'
import json,sys
s=json.load(open(sys.argv[1]))
assert s['result']=='applied' and s['committed_revision']==s['applied_revision']==int(sys.argv[2]),s
PYTHON
}
rejected() {
    local node="$1" id="$2"; shift 2
    local result=0
    "$LCS" -s "$(node_socket "$node")" --json --request-id "$id" "$@" >"$TEST_TMP/rejected.json" || result=$?
    (( result == 1 )) || die "expected rejected definition result, got $result"
    python3 - "$TEST_TMP/rejected.json" <<'PYTHON'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='rejected' and s['committed_revision'] is None and s['applied_revision'] is None,s
PYTHON
}
mutation edit-priority 4 --expected-revision 3 resource update vip1 priority=66
rejected node1 stale-edit --expected-revision 3 resource update vip1 priority=77
rejected node1 edit-priority resource update vip1 priority=77
rejected node3 witness-edit resource update vip1 priority=77
mutation create-vip 5 resource create vip2 vip address=127.0.0.204/32 interface=lo
mutation add-group 6 resource group add vips vip1 vip2
mutation group-policy 7 resource group update vips type=anti-affinity mode=best-effort
mutation remove-member 8 resource group remove vips vip2
mutation delete-group 9 resource group delete vips
mutation dependency 10 resource update vip2 depends_on=vip1
rejected node1 dangling-delete resource delete vip1
mutation clear-dependency 11 resource update vip2 depends_on=
mutation delete-vip 12 resource delete vip2
# Exact retry resolves its original base even after many later revisions.
"$LCS" -s "$(node_socket node1)" --json --request-id edit-priority resource update vip1 priority=66 >"$TEST_TMP/old-retry.json"
python3 - "$TEST_TMP/old-retry.json" <<'PYTHON'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='applied' and s['applied_revision']==4,s
PYTHON
result=0
"$LCS" -s "$(node_socket node3)" --json --request-id edit-priority resource update vip1 priority=66 >"$TEST_TMP/foreign-retry.json" || result=$?
(( result == 3 )) || die 'non-origin node claimed descriptor evidence'
grep -Fq 'original coordinator' "$TEST_TMP/foreign-retry.json" || die 'missing origin retry diagnostic'
stop_node node1
start_node node1
LCS_PIDS[0]="${LCS_PIDS[-1]}"
wait_until 40 'origin restart admission' admitted_revision node1 12
"$LCS" -s "$(node_socket node1)" --json --request-id edit-priority resource update vip1 priority=66 >"$TEST_TMP/restarted-retry.json"
# A committed edit with blocked projection returns pending, never applied/rejected.
snapshot="$(node_config node1).resources.json"
mv "$snapshot" "$TEST_TMP/before-block.json"
ln -s "$(node_config node1).state/agreement.json" "$snapshot"
for attempt in {1..20}; do
    result=0
    "$LCS" -s "$(node_socket node1)" --json --request-id blocked-admission resource update vip1 priority=99 >"$TEST_TMP/pending.json" || result=$?
    (( result != 1 )) && break
    grep -Eq 'busy|not admitted' "$TEST_TMP/pending.json" || die 'blocked edit rejected before submission'
    sleep 0.2
done
(( result == 2 )) || die "blocked committed edit returned $result"
python3 - "$TEST_TMP/pending.json" <<'PYTHON'
import json,sys
s=json.load(open(sys.argv[1])); assert s['result']=='pending' and s['committed_revision']==13 and s['applied_revision'] is None,s
PYTHON
rm "$snapshot"
for node in node1 node2 node3; do wait_until 40 "$node repaired projection" admitted_revision "$node" 13; done
# Disconnect only after original intent is published; no response is consumed.
python3 - "$LCS" "$(node_socket node1)" <<'PYTHON'
import json,socket,struct,subprocess,sys,time
cli,path=sys.argv[1:]; args=[b'resource',b'update',b'vip1',b'priority=100']
body=struct.pack('!H64sBQ65sH',1,b'detached-edit',0,0,b'',len(args))
body+=b''.join(struct.pack('!H',len(a))+a for a in args)
for attempt in range(20):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.connect(path); sock.sendall(struct.pack('!IHHII',0x4c435331,43,0,len(body),123)+body)
        end=time.monotonic()+.3
        while time.monotonic()<end:
            s=json.loads(subprocess.check_output([cli,'-s',path,'--json','config','request-status','detached-edit']))
            if s['state']!='unknown': break
            time.sleep(.05)
        if s['state']!='unknown': break
else: raise AssertionError('detached reservation was not published')
PYTHON
for node in node1 node2 node3; do wait_until 40 "$node disconnected edit" admitted_revision "$node" 14; done
"$LCS" -s "$(node_socket node1)" --json --request-id detached-edit resource update vip1 priority=100 >"$TEST_TMP/detached-retry.json"
mutation delete-last 15 resource delete vip1
assert_no_vip_ops_in_log node3
stop_node node2
stop_node node3
no_quorum() { "$LCS" -s "$(node_socket node1)" --json status | python3 -c 'import json,sys; assert not json.load(sys.stdin)["cluster"]["quorum"]'; }
wait_until 15 'quorum loss' no_quorum
rejected node1 partition-edit resource group create isolated
python3 - "$(node_config node1).state/requests.json" <<'PYTHON'
import json,sys
assert all(r['request_id'] not in ('stale-edit','dangling-delete','partition-edit') for r in json.load(open(sys.argv[1]))['requests'])
PYTHON
log 'public definition edits, groups, policy, dependencies, retry conflicts, restart and quorum fencing passed'
