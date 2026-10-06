#!/usr/bin/env python3
"""Execute the documented CLI workflow against an independent binary-v1 fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

p = argparse.ArgumentParser()
p.add_argument('--cli', default='build/cpp-release/nova-maintenance')
args = p.parse_args()
cli = str(Path(args.cli).resolve())
def u32(n): return struct.pack('<I', n)
def u64(n): return struct.pack('<Q', n)
def string(s):
    b = s.encode()
    return u32(len(b)) + b
def checked(b): return b + u32(zlib.crc32(b))
def run(*args, ok=True):
    r = subprocess.run([cli, *map(str, args)], capture_output=True, text=True)
    result = json.loads(r.stdout or r.stderr)
    assert (r.returncode == 0) == ok, (args, r.stdout, r.stderr)
    assert result['ok'] == ok, result
    return result
with tempfile.TemporaryDirectory(prefix='nova-maintenance-cli-') as tmp:
    root = Path(tmp)
    source = root / 'legacy'
    source.mkdir()
    record = checked(u32(0x4e4f5641) + u32(1) + bytes([1]) + u64(0) + u64(123) + u64(1) +
        string('a') + string('N') + string('') + string('') + u32(1) + string('v') + bytes([2]) + u64(7))
    catalog = checked(u32(0x4e434154) + u32(1) + u64(0) + u32(1) + string('N') + string('v') +
        u32(1) + string('n/a') + bytes([1]) + string('') + u32(len(record)) + record)
    (source / 'catalog.db').write_bytes(catalog)
    (source / 'wal.log').write_bytes(u32(0x4e57414c) + u32(1))
    before = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in source.iterdir()}
    assert run('inspect', source)['format'] == 'legacy-v1'
    assert run('verify', source, ok=False)['code'] == 'unsupportedVersion'
    current, backup, restored = [root / n for n in ('current', 'backup', 'restored')]
    assert run('migrate-v1', source, current)['nodes'] == 1
    assert run('inspect', current)['format'] == 'CURRENT3/catalog3/record3/WAL4'
    assert run('verify', current)['indexes'] == 1
    assert run('backup', current, backup)['nodes'] == 1
    assert run('restore', backup, restored)['nodes'] == 1
    assert run('rebuild-indexes', restored)['indexes'] == 1
    assert run('verify', restored)['nodes'] == 1
    assert run('restore', backup, restored, ok=False)['code'] == 'conflict'
    assert run('backup', current, current / 'nested', ok=False)['code'] == 'invalidArgument'
    run('verify', root / 'missing', ok=False)
    assert not (root / 'missing').exists()
    assert before == {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in source.iterdir()}
print('Maintenance CLI: all six commands, destination guards, errors and source preservation passed.')
