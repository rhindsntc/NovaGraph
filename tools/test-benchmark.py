#!/usr/bin/env python3
"""Run the benchmark executable through close/reopen and its error exit path."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('executable', type=Path)
args = parser.parse_args()
executable = str(args.executable.resolve())
result = subprocess.run([executable, '--smoke'], capture_output=True, text=True, timeout=180)
assert result.returncode == 0, f'benchmark failed ({result.returncode}):\n{result.stdout}\n{result.stderr}'
assert 'Benchmark Suite Completed!' in result.stdout, result.stdout
for number in range(1, 12):
    assert any(line.startswith(f'{number}. ') for line in result.stdout.splitlines()), result.stdout
invalid = subprocess.run([executable, '--invalid'], capture_output=True, text=True, timeout=10)
assert invalid.returncode == 1, (invalid.returncode, invalid.stderr)
assert 'Benchmark failed:' in invalid.stderr, invalid.stderr
assert 'Benchmark Suite Completed!' not in invalid.stdout
assert 'Fatal error' not in invalid.stderr
print('Benchmark contract: all 11 smoke phases complete; invalid input exits cleanly.')
