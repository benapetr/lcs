#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib.sh"
trap cleanup_cluster EXIT
umask 077
prepare_cluster
# This case intentionally starts no daemon. Validate the actual CLI against the
# same static membership/context and strict JSON parser used by the daemon.
python3 - "$LCS" "$(node_config node1)" "$TEST_TMP" <<'PY'
import copy, hashlib, json, subprocess, sys
from pathlib import Path
cli, config, directory = sys.argv[1:]
root = Path(directory)
base = {'schema_version': 1, 'revision': 0, 'cluster': 'integration',
        'groups': {'web': {'type': 'keep-together', 'mode': 'strict'}},
        'resources': {
            'public': {'type': 'vip', 'address': '192.0.2.10/24', 'interface': 'lo',
                       'group': 'web', 'home_node': 'node2'},
            'app': {'type': 'service', 'systemd_unit': 'app.service', 'group': 'web', 'depends_on': ['public']}}}
good = root/'resources.json'; good.write_text(json.dumps(base))
dynamic = copy.deepcopy(base); dynamic['schema_version'] = 2; dynamic['revision'] = 7
for resource in dynamic['resources'].values(): resource['birth_revision'] = 3
active = root/'dynamic.json'; active.write_text(json.dumps(dynamic))
def run(path, valid, data=None, context=config):
    p = subprocess.run([cli, '--config', context, '--json', 'config', 'validate', str(path)],
                       input=data, capture_output=True, timeout=5)
    assert p.returncode == (0 if valid else 1), (path, p)
    value = json.loads(p.stdout)
    assert value['valid'] is valid and not p.stderr, (path, p)
    if not valid: assert value['error']
    return value
def snapshot():
    return {p.name: (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
            for p in root.iterdir() if p.is_file()}
before = snapshot()
assert run(good, True) == {'valid': True, 'schema_version': 1, 'revision': 0, 'resource_count': 2, 'group_count': 1}
assert run(active, True)['revision'] == 7
assert run('-', True, active.read_bytes())['schema_version'] == 2
plain = subprocess.run([cli, '-c', config, 'config', 'validate', str(good)], capture_output=True, timeout=5)
assert plain.returncode == 0 and b'Resource configuration is valid' in plain.stdout and not plain.stderr
invalid = []
for key, value in [('schema_version', 3), ('cluster', 'foreign'), ('revision', -1)]:
    candidate = copy.deepcopy(base); candidate[key] = value; invalid.append(json.dumps(candidate).encode())
candidate = copy.deepcopy(base); candidate['resources']['public']['home_node'] = 'node3'; invalid.append(json.dumps(candidate).encode())
candidate = copy.deepcopy(base); candidate['resources']['app']['depends_on'] = ['missing']; invalid.append(json.dumps(candidate).encode())
candidate = copy.deepcopy(base); candidate['resources']['public']['depends_on'] = ['app']; invalid.append(json.dumps(candidate).encode())
candidate = copy.deepcopy(base); candidate['groups'] = {}; invalid.append(json.dumps(candidate).encode())
candidate = copy.deepcopy(dynamic); candidate['resources']['app']['birth_revision'] = 8; invalid.append(json.dumps(candidate).encode())
candidate = copy.deepcopy(base); candidate['resources']['public']['secret'] = 'unknown'; invalid.append(json.dumps(candidate).encode())
invalid += [b'', b'{', good.read_bytes().replace(b'"revision": 0', b'"revision": 0, "revision": 1'),
            good.read_bytes()+b' trailing', good.read_bytes()+b'\0', b'\xff']
for document in invalid: run('-', False, document)
boundary = good.read_bytes().ljust(512*1024, b' ')
assert run('-', True, boundary)['revision'] == 0
run('-', False, boundary+b' ')
run(root/'missing.json', False)
run(good, False, context=str(root/'missing.conf'))
plain = subprocess.run([cli, '-c', config, 'config', 'validate', str(root/'missing.json')], capture_output=True, timeout=5)
assert plain.returncode == 1 and not plain.stdout and b'cannot open resource JSON' in plain.stderr
for arguments in (['-c', config, 'status'], ['-c', config, 'config', 'validate'],
                  ['-c', config, 'config', 'validate', str(good), 'extra'],
                  ['-s', str(root/'absent.sock'), '-c', config, 'config', 'validate', str(good)]):
    p = subprocess.run([cli]+arguments, capture_output=True, timeout=5)
    assert p.returncode == 2 and not p.stdout, (arguments, p)
assert snapshot() == before, 'offline validation rewrote an input file'
assert not list(root.glob('*.state')) and not list(root.glob('*.resources.json')) and not list(root.glob('*.sock'))
PY
log 'offline JSON validation, schema checks, bounds and no-write regression passed'
