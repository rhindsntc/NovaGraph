#!/usr/bin/env python3
"""Explicitly operated local desktop timing gate. Evidence stays in private build/ directories.

Prepare before the quiet window. Collect two independent runs, explicitly review
and freeze their pair, then compare a new candidate against BOTH frozen reports.
Attestations record operator actions; they are not proof of an external review.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import uuid

import performance as p
from performance_lock import LOCK_PATH, measurement_lock

ROOT = p.ROOT
BUILD_FLAGS = ['-std=c++20', '-O3', '-DNDEBUG', '-target',
               platform.machine() + '-apple-macos13.0', '-isysroot', 'macosx',
               '-Icpp/include', '-Icpp/third_party', '-lsqlite3', '-pthread']
CONTROL_LIMITATIONS = ('Operator attestation is required: endpoint process/load/power/thermal '
                      'observations cannot prove that the entire window was quiet, thermal '
                      'pressure stayed normal, or no unrelated job ran. No external-human '
                      'identity or tamper-resistant signature is claimed.')


def now():
    return datetime.now(timezone.utc).isoformat()


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                    allow_nan=False).encode()).hexdigest()


def read_json(path):
    value = json.loads(Path(path).read_text(), parse_constant=lambda value: (_ for _ in ()).throw(ValueError('nonfinite JSON: ' + value)))
    if not isinstance(value, dict):
        raise ValueError('JSON evidence must be an object: ' + str(path))
    return value


def write_new(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x') as out:
        json.dump(value, out, indent=2, allow_nan=False)
        out.write('\n')


def file_record(path):
    path = Path(path).resolve(strict=True)
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'bytes': path.stat().st_size}


def verify_record(record):
    if not isinstance(record, dict) or set(record) != {'path', 'sha256', 'bytes'}:
        raise ValueError('missing file hash/size provenance')
    if type(record['bytes']) is not int or record['bytes'] < 0 or not Path(record['path']).is_absolute():
        raise ValueError('invalid file provenance')
    if file_record(record['path']) != record:
        raise ValueError('file hash/size mismatch: ' + record['path'])
    return Path(record['path'])


def budgets():
    value = read_json(ROOT / 'benchmarks/budgets.json')
    p.validate_budgets(value)
    return value


def protocol_digest():
    """Measurement identity deliberately excludes engine implementation/revision."""
    paths = [ROOT / 'tools' / name for name in
             ('performance.py', 'performance_gate.py', 'performance_lock.py', 'resource_measurements.py',
              'performance_activity.py', 'performance_diagnostic.py')]
    paths.append(ROOT / 'benchmarks/budgets.json')
    suffixes = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inc'}
    paths += [path for path in (ROOT / 'benchmarks/performance').rglob('*')
              if path.is_file() and path.suffix.lower() in suffixes]
    h = hashlib.sha256()
    for path in sorted(paths):
        h.update(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes() + b'\0')
    return h.hexdigest()


def source_identity():
    return {'revision': p.command(['git', 'rev-parse', 'HEAD'], cwd=ROOT),
            'sourceDigest': p.source_digest(),
            'dirty': bool(p.command(['git', 'status', '--porcelain', '--untracked-files=all'], cwd=ROOT)),
            'protocolDigest': protocol_digest()}


def require_clean(identity):
    if identity.get('dirty') is not False:
        raise ValueError('controlled collection requires clean source, including untracked files')
    for field, length in (('revision', 40), ('sourceDigest', 64), ('protocolDigest', 64)):
        if not re.fullmatch('[0-9a-f]{' + str(length) + '}', identity.get(field, '')):
            raise ValueError('invalid source identity: ' + field)


def environment():
    if platform.system() != 'Darwin':
        raise ValueError('this controlled hardware lane requires a native macOS desktop')
    return {'platform': 'macos', 'hardware': p.command(['sysctl', '-n', 'hw.model']),
            'os': platform.platform(), 'toolchain': p.command(['xcodebuild', '-version']),
            'compiler': p.command(['xcrun', 'clang++', '--version']),
            'sdk': p.command(['xcrun', '--sdk', 'macosx', '--show-sdk-version']),
            'architecture': platform.machine(),
            'cpu': p.command(['sysctl', '-n', 'machdep.cpu.brand_string']),
            'memoryBytes': int(p.command(['sysctl', '-n', 'hw.memsize']))}


def validate_environment(value):
    for name in ('platform', 'hardware', 'os', 'toolchain', 'compiler', 'sdk', 'architecture', 'cpu'):
        if not isinstance(value.get(name), str) or not value[name].strip():
            raise ValueError('incomplete environment identity: ' + name)
    if value['platform'] != 'macos' or type(value.get('memoryBytes')) is not int or value['memoryBytes'] <= 0:
        raise ValueError('invalid native desktop environment identity')




def observe():
    def capture(args):
        result = subprocess.run(args, text=True, capture_output=True, timeout=20)
        return {'command': args, 'returnCode': result.returncode,
                'stdout': result.stdout, 'stderr': result.stderr}
    return {'timestamp': now(), 'power': capture(['pmset', '-g', 'batt']),
            'thermal': capture(['pmset', '-g', 'therm']),
            'processes': capture(['ps', '-A', '-o', 'pid=,comm=']),
            'loadAverage': list(os.getloadavg()), 'cpuCount': os.cpu_count()}


def observation_failures(observation):
    failures = []
    commands = {'power': ['pmset', '-g', 'batt'], 'thermal': ['pmset', '-g', 'therm'],
                'processes': ['ps', '-A', '-o', 'pid=,comm=']}
    for name, command in commands.items():
        raw = observation[name]
        if raw.get('command') != command or type(raw.get('returnCode')) is not int or raw['returnCode'] != 0 or not isinstance(raw['stdout'], str):
            failures.append(name + ' observation unavailable')
    power = observation['power']['stdout']
    if not re.search(r"Now drawing from ['\"]AC Power['\"]", power):
        failures.append('AC power absent or unverifiable')
    thermal = observation['thermal']['stdout']
    normal = 'No thermal warning level has been recorded' in thermal
    explicit_level = re.search(r'(?:Thermal_Level|ThermalLevel)\s*=\s*(\d+)', thermal)
    if explicit_level:
        normal = int(explicit_level[1]) == 0
    limits = re.findall(r'(CPU_Speed_Limit|Scheduler_Limit|CPU_Available_CPUs)\s*=\s*(\d+)', thermal)
    if not normal or any(int(value) < 100 for key, value in limits if key != 'CPU_Available_CPUs'):
        failures.append('thermal state is not verifiably normal')
    cpu = observation['cpuCount']
    loads = observation['loadAverage']
    if type(cpu) is not int or cpu < 1 or not isinstance(loads, list) or len(loads) != 3 or any(type(x) not in (int, float) or not math.isfinite(x) or x < 0 for x in loads):
        failures.append('load observation unavailable')
    elif loads[0] > max(1.0, cpu * 0.25):
        failures.append('machine is busy: one-minute load exceeds the quiet threshold')
    blocked = re.compile(r'^(?:clang(?:\+\+)?(?:-\d+)?|gcc(?:-\d+)?|g\+\+|cc1(?:plus)?|swift(?:c|-frontend)?|xcodebuild|ninja|make|cmake|ctest|pytest(?:-\d+)?|Simulator|SimulatorTrampoline|launchd_sim|SpringBoard|nova-performance|native-benchmarks|xctest)$')
    for line in observation['processes']['stdout'].splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and blocked.fullmatch(Path(parts[1]).name):
            failures.append('conflicting process: ' + line.strip())
    return failures


def validate_controls(control):
    if control.get('schemaVersion') != 1 or control.get('exclusiveLock') != str(LOCK_PATH):
        raise ValueError('missing machine-global control lock metadata')
    if any(not isinstance(control.get(key), str) or not control[key].strip() for key in ('operator', 'runnerId', 'runId')):
        raise ValueError('missing operator, runner or independent run identity')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,79}', control['runnerId']):
        raise ValueError('invalid stable runner ID')
    if control.get('attestation') != {'quiet': True, 'acPower': True, 'thermal': True}:
        raise ValueError('explicit quiet/power/thermal operator attestation required')
    for stage in ('preflight', 'postflight'):
        failures = observation_failures(control[stage])
        if failures:
            raise ValueError(stage + ': ' + '; '.join(failures))
    start, end = control['sourceStart'], control['sourceEnd']
    require_clean(start)
    require_clean(end)
    if start != end:
        raise ValueError('source changed or became dirty during collection')
    if control.get('limitations') != CONTROL_LIMITATIONS:
        raise ValueError('missing control limitations')
    started = datetime.fromisoformat(control['startedAt'])
    ended = datetime.fromisoformat(control['endedAt'])
    if not started.tzinfo or not ended.tzinfo or ended <= started:
        raise ValueError('invalid measurement interval')


def prepare(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = {'schemaVersion': 1, 'kind': 'nova-controlled-build', 'complete': False,
                'createdAt': now(), 'failures': []}
    try:
        b = budgets()
        identity = source_identity()
        require_clean(identity)
        manifest.update(identity)
        manifest.update({'budgetDigest': p.budget_digest(b),
                         'protocol': p.measurement_protocol(b, 'desktop', 'measure'),
                         'environment': environment(), 'buildFlags': BUILD_FLAGS})
        # Preparation also honors the machine lock, so it cannot compile alongside collection.
        with measurement_lock():
            binary = p.build(out, False)
            if source_identity() != identity or environment() != manifest['environment']:
                raise ValueError('source/environment drifted during build')
        manifest['binary'] = file_record(binary)
        manifest['buildLog'] = file_record(out / 'build.log')
        manifest['complete'] = True
    except (ValueError, KeyError, TypeError, AttributeError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        manifest['failures'].append(str(error) or type(error).__name__)
    write_new(out / 'prepare.json', manifest)
    return manifest


def validate_prepared(manifest, b, live=False):
    if manifest.get('kind') != 'nova-controlled-build' or manifest.get('schemaVersion') != 1 or manifest.get('complete') is not True or manifest.get('failures') != []:
        raise ValueError('incomplete or invalid build manifest')
    require_clean(manifest)
    validate_environment(manifest['environment'])
    if manifest.get('budgetDigest') != p.budget_digest(b) or manifest.get('protocolDigest') != protocol_digest():
        raise ValueError('stale build budget or measurement protocol')
    if manifest.get('protocol') != p.measurement_protocol(b, 'desktop', 'measure') or manifest.get('buildFlags') != BUILD_FLAGS:
        raise ValueError('incompatible build flags or measurement protocol')
    verify_record(manifest['binary'])
    verify_record(manifest['buildLog'])
    if live:
        identity = source_identity()
        require_clean(identity)
        if any(manifest[key] != value for key, value in identity.items()):
            raise ValueError('built source is stale or differs from clean checkout')
        if manifest['environment'] != environment():
            raise ValueError('build environment differs from current environment')


def collect(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {'schemaVersion': 1, 'profile': 'desktop', 'complete': False,
              'workloads': [], 'runFailures': [], 'createdAt': now()}
    try:
        b = budgets()
        prepared_path = args.prepared.resolve() / 'prepare.json'
        prepared = read_json(prepared_path)
        validate_prepared(prepared, b, live=True)
        if not args.operator.strip() or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,79}', args.runner_id):
            raise ValueError('operator and stable non-sensitive runner ID required')
        if not (args.attest_quiet and args.attest_ac_power and args.attest_thermal):
            raise ValueError('explicit quiet/power/thermal attestations required')
        for key in ('revision', 'sourceDigest', 'dirty', 'protocolDigest', 'budgetDigest', 'protocol', 'environment', 'buildFlags'):
            report[key] = prepared[key]
        report['prepared'] = file_record(prepared_path)
        control = {'schemaVersion': 1, 'runId': str(uuid.uuid4()), 'runnerId': args.runner_id,
                   'operator': args.operator, 'attestation': {'quiet': True, 'acPower': True, 'thermal': True},
                   'exclusiveLock': str(LOCK_PATH), 'limitations': CONTROL_LIMITATIONS}
        report['control'] = control
        with measurement_lock():
            try:
                control['startedAt'] = now()
                control['preflight'] = observe()
                failures = observation_failures(control['preflight'])
                if failures:
                    raise ValueError('preflight: ' + '; '.join(failures))
                control['sourceStart'] = source_identity()
                require_clean(control['sourceStart'])
                validate_prepared(prepared, b, live=True)
                # Build/validation completed before this point; no compilation in this window.
                p.collect_workloads([prepared['binary']['path']], report, out)
            finally:
                control['sourceEnd'] = source_identity()
                control['postflight'] = observe()
                control['endedAt'] = now()
            validate_controls(control)
            validate_prepared(prepared, b, live=True)
        report['complete'] = not report['runFailures']
        report['evaluation'] = p.evaluate(report, b)
        report['regressionAcceptance'] = 'not-evaluated-no-frozen-baseline'
    except (ValueError, KeyError, TypeError, AttributeError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        report['complete'] = False
        report['runFailures'].append({'kind': 'controlled-collection', 'error': str(error) or type(error).__name__})
        report['evaluation'] = {'status': 'failed', 'hardwareQualified': False, 'failures': report['runFailures']}
    write_new(out / 'results.json', report)
    return report


def validate_report(report, b):
    """Recompute absolute acceptance; never trust the saved evaluation label."""
    if report.get('kind') == 'nova-activity-diagnostic':
        raise ValueError('activity diagnostic cannot qualify controlled evidence')
    require_clean(report)
    if report.get('profile') != 'desktop' or report['environment'].get('platform') != 'macos':
        raise ValueError('controlled local gate requires native desktop hardware evidence')
    if report.get('protocol') != p.measurement_protocol(b, 'desktop', 'measure'):
        raise ValueError('smoke, resources, incomplete or incompatible protocol cannot qualify')
    if report.get('budgetDigest') != p.budget_digest(b) or report.get('protocolDigest') != protocol_digest():
        raise ValueError('incompatible budget or measurement protocol digest')
    validate_controls(report['control'])
    identity = report['control']['sourceStart']
    if any(report.get(key) != value for key, value in identity.items()):
        raise ValueError('report and collection source provenance differ')
    prepared = read_json(verify_record(report['prepared']))
    validate_prepared(prepared, b)
    for key in ('revision', 'sourceDigest', 'protocolDigest', 'budgetDigest', 'protocol', 'environment', 'buildFlags'):
        if report.get(key) != prepared.get(key):
            raise ValueError('report/build provenance mismatch: ' + key)
    absolute = p.evaluate(report, b)
    if absolute['status'] != 'passed' or absolute.get('hardwareQualified') is not True:
        raise ValueError('absolute acceptance failed: ' + '; '.join(map(str, absolute.get('failures', []))))
    return absolute


def comparable_identity(report):
    return {key: report[key] for key in ('environment', 'profile', 'protocol', 'protocolDigest', 'budgetDigest', 'buildFlags')} | {'runnerId': report['control']['runnerId']}


def repeatability_failures(first, second, b):
    """Both directions; a faster run cannot be selected or hide an unstable pair."""
    failures = []
    for current, baseline in ((first, second), (second, first)):
        result = p.evaluate(current, b, baseline)
        if result['status'] != 'passed':
            failures.extend(result.get('failures') or ['baseline pair failed repeatability'])
    return failures


def validate_pair(reports, b):
    if len(reports) != 2:
        raise ValueError('exactly two complete independent baseline reports are required')
    for report in reports:
        validate_report(report, b)
    first, second = reports
    if comparable_identity(first) != comparable_identity(second) or first['revision'] != second['revision'] or first['sourceDigest'] != second['sourceDigest']:
        raise ValueError('baseline pair must use the same frozen clean revision and environment')
    if first['control']['runId'] == second['control']['runId']:
        raise ValueError('baseline reports are not independent collections')
    intervals = sorted((datetime.fromisoformat(r['control']['startedAt']), datetime.fromisoformat(r['control']['endedAt'])) for r in reports)
    if intervals[0][1] > intervals[1][0]:
        raise ValueError('baseline collection intervals overlap')
    failures = repeatability_failures(first, second, b)
    if failures:
        raise ValueError('baseline repeatability exceeds reviewed 10%: ' + '; '.join(failures))


def review(args):
    if not args.approve or not args.reviewer.strip() or not args.approval_note.strip():
        raise ValueError('freezing requires explicit --approve, --reviewer and --approval-note')
    b = budgets()
    records = [file_record(path) for path in args.reports]
    if records[0]['path'] == records[1]['path']:
        raise ValueError('two independent baseline report paths are required')
    reports = [read_json(verify_record(record)) for record in records]
    validate_pair(reports, b)
    first = reports[0]
    manifest = {'schemaVersion': 1, 'kind': 'nova-frozen-controlled-baseline',
                'reports': records, 'revision': first['revision'], 'sourceDigest': first['sourceDigest'],
                'identity': comparable_identity(first),
                'approval': {'action': 'explicit-operator-review', 'reviewer': args.reviewer,
                             'note': args.approval_note, 'approvedAt': now(),
                             'externalHumanIdentityVerified': False},
                'repeatabilityFraction': b['regressionFraction']}
    manifest['manifestDigest'] = digest(manifest)
    write_new(args.output, manifest)
    return manifest


def load_baseline(path, b):
    manifest = read_json(path)
    signed = {key: value for key, value in manifest.items() if key != 'manifestDigest'}
    if manifest.get('manifestDigest') != digest(signed):
        raise ValueError('frozen baseline manifest hash mismatch')
    if manifest.get('kind') != 'nova-frozen-controlled-baseline' or manifest.get('schemaVersion') != 1:
        raise ValueError('invalid frozen baseline manifest')
    approval = manifest['approval']
    if approval.get('action') != 'explicit-operator-review' or not approval.get('reviewer') or not approval.get('note') or approval.get('externalHumanIdentityVerified') is not False:
        raise ValueError('missing explicit baseline review metadata')
    if not datetime.fromisoformat(approval['approvedAt']).tzinfo:
        raise ValueError('invalid baseline approval date')
    reports = [read_json(verify_record(record)) for record in manifest['reports']]
    validate_pair(reports, b)
    first = reports[0]
    if manifest['identity'] != comparable_identity(first) or manifest['revision'] != first['revision'] or manifest['sourceDigest'] != first['sourceDigest'] or manifest['repeatabilityFraction'] != b['regressionFraction']:
        raise ValueError('frozen baseline identity does not match its reports')
    return reports


def gate(args):
    result = {'schemaVersion': 1, 'kind': 'nova-controlled-gate', 'createdAt': now(),
              'status': 'failed', 'absoluteAcceptance': 'not-evaluated',
              'regressionAcceptance': 'not-evaluated', 'hardwareQualified': False,
              'failures': [], 'comparisons': []}
    try:
        b = budgets()
        result['candidate'] = file_record(args.candidate)
        candidate = read_json(verify_record(result['candidate']))
        result['absoluteEvaluation'] = validate_report(candidate, b)
        result['absoluteAcceptance'] = 'passed'
        result['hardwareQualified'] = True
        if args.baseline is None:
            raise ValueError('regression acceptance requires an explicitly reviewed frozen baseline')
        result['baseline'] = file_record(args.baseline)
        baselines = load_baseline(args.baseline, b)
        for index, baseline in enumerate(baselines):
            if comparable_identity(candidate) != comparable_identity(baseline):
                raise ValueError('candidate environment, runner, build flags, budget or protocol differs from baseline')
            if candidate['control']['runId'] == baseline['control']['runId']:
                raise ValueError('candidate must be an independent collection, not a baseline replay')
            comparison = p.evaluate(candidate, b, baseline)
            result['comparisons'].append({'baselineReport': index + 1, 'evaluation': comparison})
            if comparison['status'] != 'passed':
                result['failures'].extend(comparison.get('failures') or ['regression failed'])
        result['regressionAcceptance'] = 'failed' if result['failures'] else 'passed'
        result['status'] = result['regressionAcceptance']
    except (ValueError, KeyError, TypeError, AttributeError, IndexError, OSError) as error:
        result['failures'].append(str(error))
        if result['absoluteAcceptance'] == 'not-evaluated':
            result['absoluteAcceptance'] = 'failed-or-unqualified'
    write_new(args.output, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    prep = sub.add_parser('prepare', help='compile and record clean-source build provenance before a quiet window')
    prep.add_argument('--output', type=Path, required=True)
    col = sub.add_parser('collect', help='collect native desktop evidence; plan a quiet window (100 processes, potentially 45+ minutes)')
    col.add_argument('--prepared', type=Path, required=True)
    col.add_argument('--output', type=Path, required=True)
    col.add_argument('--runner-id', required=True)
    col.add_argument('--operator', required=True)
    for flag in ('quiet', 'ac-power', 'thermal'):
        col.add_argument('--attest-' + flag, action='store_true', required=True)
    diag = sub.add_parser('diagnose', help='six-process activity diagnostic; never qualifies a baseline')
    diag.add_argument('--prepared', type=Path, required=True)
    diag.add_argument('--output', type=Path, required=True)
    for flag in ('quiet', 'ac-power', 'thermal'):
        diag.add_argument('--attest-' + flag, action='store_true', required=True)
    rev = sub.add_parser('review', help='explicitly approve a passing repeatable pair; never automatic')
    rev.add_argument('--reports', type=Path, nargs=2, required=True)
    rev.add_argument('--output', type=Path, required=True)
    rev.add_argument('--reviewer', required=True)
    rev.add_argument('--approval-note', required=True)
    rev.add_argument('--approve', action='store_true', required=True)
    comparison = sub.add_parser('gate', help='compare a candidate against both frozen baseline reports')
    comparison.add_argument('--candidate', type=Path, required=True)
    comparison.add_argument('--baseline', type=Path)
    comparison.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        from performance_diagnostic import diagnose
        value = {'diagnose': diagnose, 'prepare': prepare, 'collect': collect, 'review': review, 'gate': gate}[args.action](args)
        print(json.dumps(value.get('evaluation', value), indent=2, allow_nan=False))
        if args.action == 'diagnose':
            return 0 if value['diagnosticComplete'] else 1
        if args.action in ('prepare', 'collect'):
            return 0 if value['complete'] and value.get('evaluation', {}).get('status', 'passed') == 'passed' else 1
        return 0 if args.action == 'review' or value['status'] == 'passed' else 1
    except (ValueError, KeyError, TypeError, AttributeError, IndexError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({'status': 'failed', 'action': args.action, 'error': str(error) or type(error).__name__}), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
