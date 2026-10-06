#!/usr/bin/env python3
"""Separate SIGKILL and volatile/durable-filesystem campaigns; retain reproducible evidence."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--worker', default='build/cpp-release/nova_crash_worker')
parser.add_argument('--traces', type=int, default=1000)
parser.add_argument('--process-traces', type=int, default=64)
parser.add_argument('--output', default='build/recovery-evidence.json')
args = parser.parse_args()
if args.traces < 1000 or args.process_traces < 1:
    parser.error('campaign requires at least 1000 model traces and one process trace')
worker = str(Path(args.worker).resolve())
started = time.time()
evidence = {'schemaVersion': 1, 'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
            'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], text=True).strip()),
            'scope': 'small mutable graph, indexed/scanned filters, adjacency, tiering, two retained roots and GC',
            'modeledPowerLoss': {}, 'processCrash': {}, 'failures': []}
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
try:
    fixtures = json.loads(Path('cpp/tests/faults/seeds.json').read_text())
    for seed in fixtures['modelSeeds']:
        subprocess.run([worker, 'model-seed', str(seed)], check=True)
    model = subprocess.run([worker, 'model', str(args.traces)], text=True, stdout=subprocess.PIPE, check=True)
    evidence['modeledPowerLoss'] = json.loads(model.stdout.strip())
    with tempfile.TemporaryDirectory(prefix='nova-process-campaign-') as temporary:
        base = Path(temporary)
        baseline = subprocess.check_output([worker, 'trace', str(base / 'baseline')], text=True)
        events = [(int(line.split()[1]), line.split()[2]) for line in baseline.splitlines() if line.startswith('EVENT ')]
        first_by_name = {}
        for index, name in events:
            first_by_name.setdefault(name, index)
        boundaries = sorted(set(first_by_name.values()) | {1 + (seed * 7919) % len(events) for seed in range(args.process_traces)})
        # Always include permanent regression boundaries, even in a reduced local campaign.
        fixtures = json.loads(Path('cpp/tests/faults/seeds.json').read_text())
        boundaries = sorted(set(boundaries) | set(fixtures['processBoundaries']))
        observed = []
        for seed, boundary in enumerate(boundaries):
            directory = base / f'trace-{seed}'
            run = subprocess.run([worker, 'write', str(directory), str(boundary)], text=True, capture_output=True)
            if run.returncode != -9:
                raise RuntimeError(f'process seed={seed} boundary={boundary} expected SIGKILL, got {run.returncode}: {run.stderr}')
            acknowledgments = [int(line.split()[1]) for line in run.stdout.splitlines() if line.startswith('ACK ')]
            # Parent-owned evidence is outside the database and synced before reopening it.
            with (base / f'ack-{seed}.json').open('w') as file:
                json.dump(acknowledgments, file)
                file.flush()
                os.fsync(file.fileno())
            recovered = json.loads(subprocess.check_output([worker, 'verify', str(directory)], text=True))
            last = acknowledgments[-1] if acknowledgments else -1
            if recovered['epoch'] not in (last, last + 1) or not recovered['indexesEquivalent']:
                raise RuntimeError(f'process seed={seed} boundary={boundary} ack={last} recovered={recovered}')
            observed.append({'seed': seed, 'boundary': boundary, 'lastAcknowledged': last, 'recovered': recovered['epoch']})
            if (seed + 1) % 10 == 0:
                print(f'process traces {seed + 1}/{len(boundaries)}', flush=True)
        evidence['processCrash'] = {'traces': len(observed), 'signal': 'SIGKILL', 'boundaries': len(events),
            'namedPoints': sorted(first_by_name), 'acknowledgedLosses': 0, 'partialTransactions': 0, 'observed': observed}
except Exception as error:
    evidence['failures'].append(str(error))
    raise
finally:
    evidence['elapsedSeconds'] = round(time.time() - started, 3)
    output.write_text(json.dumps(evidence, indent=2) + '\n')
print(f'Recovery evidence: {output}', flush=True)
