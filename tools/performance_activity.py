"""Sampled process CPU attribution for private diagnostics, not load qualification."""
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import subprocess
import threading
import time

INTERVAL_SECONDS = 2.0


def now():
    return datetime.now(timezone.utc).isoformat()


def parse_processes(raw):
    rows = []
    for line in raw.splitlines():
        fields = line.split(None, 8)
        if len(fields) != 9:
            raise ValueError('incomplete process observation')
        pid, ppid = int(fields[0]), int(fields[1])
        value = fields[2]
        if not re.fullmatch(r'\d+:\d{2}(?:\.\d+)?|\d+:\d{2}:\d{2}(?:\.\d+)?', value):
            raise ValueError('invalid process CPU time')
        parts = [float(x) for x in value.split(':')]
        if any(x >= 60 for x in parts[1:]):
            raise ValueError('invalid process CPU time')
        cpu = sum(x * 60 ** i for i, x in enumerate(reversed(parts)))
        if not math.isfinite(cpu) or pid <= 0 or ppid < 0:
            raise ValueError('invalid process accounting')
        rows.append(dict(pid=pid, ppid=ppid, cpuSeconds=cpu,
                         started=' '.join(fields[3:8]), command=fields[8]))
    return rows


def identity(process):
    return process['pid'], process['started'], process['command']


def classify(process, owners):
    return owners.get(identity(process), 'other')


def read_process(pid):
    result = subprocess.run(['ps', '-ww', '-p', str(pid), '-o', 'pid=,ppid=,time=,lstart=,comm='],
                            capture_output=True, text=True, timeout=10, check=True)
    rows = parse_processes(result.stdout)
    if len(rows) != 1 or rows[0]['pid'] != pid:
        raise ValueError('owned process identity unavailable')
    return rows[0]


def capture_sample(phase, owners):
    start = time.monotonic()
    result = subprocess.run(['ps', '-ww', '-A', '-o', 'pid=,ppid=,time=,lstart=,comm='],
                            capture_output=True, text=True, timeout=10, check=True)
    rows = parse_processes(result.stdout)
    if not rows:
        raise ValueError('empty process observation')
    for row in rows:
        row['role'] = classify(row, owners)
    return dict(timestamp=now(), monotonic=start, phase=phase,
                loadAverage=list(os.getloadavg()), processes=rows,
                samplingSeconds=time.monotonic() - start)


def summarize(samples):
    """CPU seconds from matching identities only; gaps and boundary intervals stay unassigned."""
    result = dict(samples=len(samples), coverageComplete=bool(samples), sampleErrors=[],
                  gaps=[], maxGapSeconds=0., unmatchedProcessObservations=0, ownershipTransitions=0, phases={})
    costs = []
    previous = None
    for sample in samples:
        if previous is not None:
            gap = sample['monotonic'] - previous['monotonic']
            result['maxGapSeconds'] = max(result['maxGapSeconds'], gap)
            if gap <= 0 or gap > INTERVAL_SECONDS * 1.5:
                result['gaps'].append(dict(after=previous['monotonic'], seconds=gap))
        if 'error' in sample:
            result['sampleErrors'].append(sample['error'])
            previous = sample
            continue
        costs.append(sample['samplingSeconds'])
        phase = result['phases'].setdefault(sample['phase'], dict(
            cpuSecondsByRole={'nova': 0., 'collector': 0., 'other': 0.},
            cpuSecondsByCommand={}, observedSeconds=0., loads=[]))
        phase['loads'].append(sample['loadAverage'][0])
        if previous and 'error' not in previous and previous['phase'] == sample['phase']:
            gap = sample['monotonic'] - previous['monotonic']
            if 0 < gap <= INTERVAL_SECONDS * 1.5:
                phase['observedSeconds'] += gap
                before = {identity(row): row for row in previous['processes']}
                after = {identity(row): row for row in sample['processes']}
                result['unmatchedProcessObservations'] += len(before.keys() ^ after.keys())
                for key in before.keys() & after.keys():
                    row = after[key]
                    delta = row['cpuSeconds'] - before[key]['cpuSeconds']
                    if delta < 0:
                        result['sampleErrors'].append('CPU counter decreased within identity')
                        continue
                    if row['role'] != before[key]['role']:
                        result['ownershipTransitions'] += 1
                        continue
                    phase['cpuSecondsByRole'][row['role']] += delta
                    command = row['command']
                    phase['cpuSecondsByCommand'][command] = phase['cpuSecondsByCommand'].get(command, 0.) + delta
        previous = sample
    result['coverageComplete'] = bool(samples) and not result['sampleErrors'] and not result['gaps']
    result['samplingCostSeconds'] = dict(mean=sum(costs)/len(costs) if costs else None,
                                        maximum=max(costs) if costs else None)
    return result


class ActivityMonitor:
    def __init__(self, path):
        self.path = Path(path)
        self.phase = 'idle-before'
        self.owners = {}
        self.guard = threading.Lock()
        self.stopped = threading.Event()
        self.first = threading.Event()
        self.errors = []
        self.thread = None

    def register(self, pid, role, expected_command=None):
        row = read_process(pid)
        if expected_command is not None and Path(row['command']).resolve() != Path(expected_command).resolve():
            raise ValueError('owned executable identity mismatch')
        if role not in ('nova', 'collector'):
            raise ValueError('unsupported owner role')
        with self.guard:
            self.owners[identity(row)] = role
        return row

    def set_phase(self, phase):
        with self.guard:
            self.phase = phase

    def start(self):
        self.register(os.getpid(), 'collector')
        # Open synchronously so overwrite/permission failures cannot hide in the thread.
        stream = self.path.open('x')
        self.thread = threading.Thread(target=self._record, args=(stream,), daemon=True)
        self.thread.start()
        if not self.first.wait(12):
            raise RuntimeError('activity sampler did not start')

    def _record(self, stream):
        try:
            with stream:
                while not self.stopped.is_set():
                    start = time.monotonic()
                    with self.guard:
                        phase, owners = self.phase, dict(self.owners)
                    try:
                        sample = capture_sample(phase, owners)
                    except Exception as error:
                        sample = dict(timestamp=now(), monotonic=start, phase=phase,
                                      error=type(error).__name__ + ': ' + str(error))
                    stream.write(json.dumps(sample, allow_nan=False) + '\n')
                    stream.flush()
                    self.first.set()
                    self.stopped.wait(max(0, INTERVAL_SECONDS - (time.monotonic() - start)))
        except Exception as error:
            self.errors.append(type(error).__name__ + ': ' + str(error))
        finally:
            self.first.set()

    def stop(self):
        self.stopped.set()
        if self.thread:
            self.thread.join(timeout=12)
            if self.thread.is_alive():
                raise RuntimeError('activity sampler did not stop')
