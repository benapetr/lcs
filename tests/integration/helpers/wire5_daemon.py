#!/usr/bin/env python3
"""Build a cached current-source wire-5 daemon for pre-upgrade INI tests.

Only the advertised maximum peer version changes; production sources and the
normal daemon remain untouched. This exercises the real capability gate rather
than adding a production switch that disables automatic migration.
"""
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
files = sorted([root / 'Makefile', *root.glob('src/*.c'), *root.glob('src/*.h')])
digest = hashlib.sha256()
for path in files:
    digest.update(str(path.relative_to(root)).encode() + b'\0' + path.read_bytes())
for name in ('CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'PKG_CONFIG_PATH', 'PKG_CONFIG_SYSROOT_DIR'):
    digest.update(name.encode() + b'\0' + os.environ.get(name, '').encode() + b'\0')
cache = Path(tempfile.gettempdir()) / f'lcs-wire5-fixtures-{os.getuid()}'
cache.mkdir(mode=0o700, exist_ok=True)
target = cache / digest.hexdigest()
with (cache / 'build.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    if not (target / 'lcsd').is_file():
        staging = Path(tempfile.mkdtemp(prefix='build-', dir=cache))
        try:
            (staging / 'src').mkdir()
            for path in files:
                shutil.copy2(path, staging / path.relative_to(root))
            protocol = staging / 'src/protocol.h'
            text = protocol.read_text()
            old = '#define LCS_PEER_PROTO_VERSION 6'
            assert text.count(old) == 1, 'update the wire-5 fixture for the current protocol declaration'
            protocol.write_text(text.replace(old, '#define LCS_PEER_PROTO_VERSION 5'))
            subprocess.run(['make', '-s', '-j2', 'lcsd'], cwd=staging, check=True, stdout=subprocess.DEVNULL)
            staging.rename(target)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
print(target / 'lcsd')
