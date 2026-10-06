"""Short, explicitly unqualified desktop activity diagnostic."""
import contextlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

import performance as p
import performance_activity as activity
from performance_lock import measurement_lock

KIND = 'nova-activity-diagnostic'
ORDER = ('navigation', 'recommendations') * 3
LIMITATIONS = [
    'Instrumented diagnostic only: cannot qualify a baseline or regression gate.',
    'Sampling adds overhead and misses processes that exit between observations.',
    'CPU seconds are not machine utilization percentages or proof of causality.',
    'Other processes may be reacting to Nova; their activity is not necessarily independent interference.',
    'CPU/load observations do not attribute physical disk I/O.',
    'Native samples have no wall-clock timestamps; correlation is only at whole-process granularity.',
    'Power and thermal observations are endpoints, not continuous coverage.',
    'Cold reads are cold in Nova payload memory; the OS filesystem cache is uncontrolled.',
]


def observe():
    import performance_gate as g
    raw = g.observe()
    return dict(raw=raw, gateFailures=g.observation_failures(raw))


def idle(seconds):
    time.sleep(seconds)


def run_child(command, stdout, stderr, on_start, timeout=180):
    """Own a process group so interrupted/timed-out runs cannot leave benchmark children."""
    with Path(stdout).open('x') as out, Path(stderr).open('x') as err:
        child = subprocess.Popen(command, stdout=out, stderr=err, cwd=p.ROOT, start_new_session=True)
        try:
            on_start(child)
            return child.wait(timeout=timeout)
        finally:
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()


def native_summary(native, workload):
    p.validate_mutation_protocol(native)
    data = native['dataset']
    if native['platform'] != 'macos' or data['name'] != workload or data['nodes'] != 10000 or data['seed'] != 42:
        raise ValueError('diagnostic native dataset/platform mismatch')
    metrics = {}
    for name in p.METRICS:
        values = native['metrics'][name]
        if len(values) != (100 if name in p.SAMPLED_METRICS else 1):
            raise ValueError('diagnostic sample count mismatch')
        metrics[name] = dict(p50=p.percentile(values, .5), p95=p.percentile(values, .95))
    p.percentile([native['peakRSSBytes']], 1)
    return dict(metrics=metrics, peakRSSBytes=native['peakRSSBytes'])


def timing_spread(runs):
    result = {}
    for workload in sorted({run['workload'] for run in runs if 'metrics' in run}):
        rows = [r for r in runs if r['workload'] == workload and 'metrics' in r]
        result[workload] = {}
        for metric in rows[0]['metrics']:
            result[workload][metric] = {}
            for quantile in ('p50', 'p95'):
                values = [r['metrics'][metric][quantile] for r in rows]
                lo, hi = min(values), max(values)
                result[workload][metric][quantile] = dict(
                    processes=len(values), minimum=lo, maximum=hi,
                    maxVsMinPercent=100*(hi/lo-1) if lo > 0 else None)
    return result


@contextlib.contextmanager
def interruption_handlers():
    """Route ordinary external cancellation through child cleanup and evidence retention."""
    previous = {}
    def interrupt(signum, frame):
        raise KeyboardInterrupt(signal.Signals(signum).name)
    try:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            previous[signum] = signal.signal(signum, interrupt)
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def diagnose(args):
    with interruption_handlers():
        return _diagnose(args)


def _diagnose(args):
    import performance_gate as g
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = dict(schemaVersion=1, kind=KIND, complete=False, hardwareQualified=False,
                  status='unqualified-diagnostic', diagnosticComplete=False,
                  startedAt=g.now(), errors=[], runs=[], observations={}, limitations=LIMITATIONS,
                  diagnosticProtocol=dict(version=1, idleBeforeSeconds=120, recoverySeconds=120,
                      sampleIntervalSeconds=2, order=list(ORDER), nodes=10000, seed=42, samples=100, warmup=20),
                  qualification='ineligible-diagnostic', attestation={
                      key: getattr(args, 'attest_' + key, False) for key in ('quiet', 'ac_power', 'thermal')})
    monitor = None
    try:
        if not all(value is True for value in report['attestation'].values()):
            raise ValueError('explicit quiet, AC power and thermal attestations required')
        prepared_path = args.prepared.resolve() / 'prepare.json'
        prepared = g.read_json(prepared_path)
        g.validate_prepared(prepared, g.budgets(), live=True)
        report['prepared'] = g.file_record(prepared_path)
        report['sourceStart'] = g.source_identity()
        with measurement_lock():
            monitor = activity.ActivityMonitor(out / 'activity.jsonl')
            try:
                monitor.start()
                report['observations']['idleStart'] = observe()
                print('Diagnostic idle observation: 120 seconds.', flush=True)
                idle(120)
                report['observations']['beforeActive'] = observe()
                failures = report['observations']['beforeActive'].get('gateFailures', [])
                if failures:
                    raise ValueError('readiness after idle: ' + '; '.join(failures))
                for index, workload in enumerate(ORDER, 1):
                    name = str(index) + '-' + workload
                    monitor.set_phase(name)
                    entry = dict(index=index, workload=workload, startedAt=g.now(), startMonotonic=time.monotonic())
                    report['runs'].append(entry)
                    command = [prepared['binary']['path'], workload, '10000', '42', '100', '20']
                    entry['command'] = command
                    def register(child):
                        entry['identity'] = monitor.register(child.pid, 'nova', command[0])
                    try:
                        entry['returnCode'] = run_child(command, out/(name+'.stdout.json'), out/(name+'.stderr'), register)
                    finally:
                        entry.update(endedAt=g.now(), endMonotonic=time.monotonic())
                    if entry['returnCode'] != 0:
                        raise ValueError('native process failed: ' + name)
                    if (out/(name+'.stderr')).stat().st_size:
                        raise ValueError('unexpected native stderr: ' + name)
                    entry.update(native_summary(g.read_json(out/(name+'.stdout.json')), workload))
                    print(f'Diagnostic process {index}/6 completed: {workload}.', flush=True)
                monitor.set_phase('recovery')
                report['observations']['immediatePostflight'] = observe()
                print('Diagnostic recovery observation: 120 seconds.', flush=True)
                idle(120)
                report['observations']['recoveryEnd'] = observe()
            finally:
                monitor.stop()
            g.validate_prepared(prepared, g.budgets(), live=True)
    except (Exception, KeyboardInterrupt) as error:
        report['errors'].append(type(error).__name__ + ': ' + str(error))
    finally:
        if monitor:
            report['errors'].extend(monitor.errors)
        try:
            report['sourceEnd'] = g.source_identity()
            if 'sourceStart' in report and report['sourceStart'] != report['sourceEnd']:
                report['errors'].append('source identity changed during diagnostic')
            if (out/'activity.jsonl').exists():
                samples = [json.loads(line) for line in (out/'activity.jsonl').read_text().splitlines()]
                report['activitySummary'] = activity.summarize(samples)
                if not report['activitySummary']['coverageComplete']:
                    report['errors'].append('activity coverage incomplete; see gaps and sample errors')
            report['timingSpread'] = timing_spread(report['runs'])
        except Exception as error:
            report['errors'].append('summary: ' + type(error).__name__ + ': ' + str(error))
        report['endedAt'] = g.now()
        report['diagnosticComplete'] = not report['errors'] and len(report['runs']) == 6 and 'recoveryEnd' in report['observations']
        report['files'] = [g.file_record(path) for path in sorted(out.iterdir()) if path.is_file()]
        g.write_new(out/'diagnostic.json', report)
    return report
