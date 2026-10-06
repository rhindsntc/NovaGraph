#!/usr/bin/env python3
"""Collect revision-bound macOS Swift/C hot-read and public Swift operation diagnostics; separately evaluate approved fixture caps."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LANES = ('cTransportUs', 'cJSONUs', 'swiftTypedUs')
HISTORICAL_PROTOCOL = {'runs': 20, 'samples': 100, 'warmup': 20, 'nodes': 32, 'payloadBytes': [64, 512, 1024]}
OPERATION_LANES = ('swiftColdUs', 'swiftSingleMutationUs', 'swiftBatch250Us', 'swiftTraversalUs', 'swiftAsyncReadUs', 'swiftAsyncTraversalUs', 'swiftAsyncMutationUs')
OPERATIONS = {'version': 1, 'mode': 'measure', 'warmup': 20, 'samples': 100, 'batchMutations': 250, 'traversalNodes': 32, 'traversalDepth': 2, 'traversalLimit': 100, 'graph': 'directed-ring', 'cold': 'unique-after-forced-trim', 'durability': 'default-durable-receipt', 'mutation': 'replace-string-and-integer-properties', 'asyncConcurrency': 1, 'phaseOrder': ['cold', 'single', 'batch', 'traversal', 'asyncRead', 'asyncTraversal', 'asyncMutation']}
PROTOCOL = {**HISTORICAL_PROTOCOL, 'operations': OPERATIONS}
PROTOCOL_DIGEST = hashlib.sha256(json.dumps(PROTOCOL, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def exact(actual, expected):
    # Python bool compares equal to 1; protocol identities must preserve types.
    return json.dumps(actual, sort_keys=True, allow_nan=False) == json.dumps(expected, sort_keys=True, allow_nan=False)

spec = importlib.util.spec_from_file_location('performance', ROOT/'tools/performance.py')
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)

def validate_run(run, offset, expanded=False):
    if not isinstance(run, dict): raise ValueError('missing run')
    expected = {'schemaVersion': 3 if expanded else 2, 'validated': True, 'orderOffset': offset, 'warmup': 20, 'samples': 100}
    if any(type(run.get(k)) is not type(v) or run[k] != v for k, v in expected.items()):
        raise ValueError('invalid Swift run protocol or validation')
    scenarios = run.get('scenarios')
    if not isinstance(scenarios, list) or len(scenarios) != 3: raise ValueError('missing scenarios')
    for scenario, size in zip(scenarios, PROTOCOL['payloadBytes']):
        if not isinstance(scenario, dict): raise ValueError('invalid scenario')
        for key, value in [('payloadBytes', size), ('nodes', 32), ('visitedNodes', 32)]:
            if type(scenario.get(key)) is not int or scenario[key] != value: raise ValueError('invalid scenario shape')
        for lane in LANES:
            values = scenario.get(lane)
            if not isinstance(values, list) or len(values) != 100: raise ValueError('missing lane samples')
            performance.percentile(values, .95)
    if expanded:
        if not exact(run.get('operationsProtocol'), OPERATIONS): raise ValueError('invalid Swift operations protocol')
        scenarios = run.get('operationScenarios')
        if not isinstance(scenarios, list) or len(scenarios) != 3: raise ValueError('missing Swift operation scenarios')
        for scenario, size in zip(scenarios, PROTOCOL['payloadBytes']):
            shape = {'payloadBytes': size, 'coldNodes': 120, 'coldEvicted': 120, 'coldVisited': 120, 'reopenValidated': True, 'batchMutations': 250, 'traversalNodes': 32, 'traversalEdges': 32, 'traversalResults': 2}
            if not isinstance(scenario, dict) or any(type(scenario.get(k)) is not type(v) or scenario[k] != v for k,v in shape.items()): raise ValueError('invalid Swift operation scenario')
            for lane in OPERATION_LANES:
                values = scenario.get(lane)
                if not isinstance(values, list) or len(values) != 100: raise ValueError('missing Swift operation samples')
                performance.percentile(values, .95)

def summarize(report):
    expanded = report.get('schemaVersion') == 2 and report.get('kind') == 'swift-diagnostic'
    if not expanded and (report.get('schemaVersion') != 1 or report.get('kind') != 'swift-read-diagnostic'): raise ValueError('unknown Swift report')
    if type(report.get('schemaVersion')) is not int: raise ValueError('invalid Swift schema version')
    if expanded and report.get('protocolDigest') != PROTOCOL_DIGEST: raise ValueError('invalid Swift protocol digest')
    if report.get('complete') is not True or report.get('runFailures') != []: raise ValueError('incomplete Swift report')
    if type(report.get('dirty')) is not bool: raise ValueError('missing cleanliness')
    for key, length in [('revision', 40), ('sourceDigest', 64)]:
        if not isinstance(report.get(key), str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}', report[key]): raise ValueError('invalid provenance')
    if not exact(report.get('protocol'), PROTOCOL if expanded else HISTORICAL_PROTOCOL): raise ValueError('unexpected Swift protocol')
    env = report.get('environment', {})
    if env.get('platform') != 'macos' or env.get('configuration') != 'release': raise ValueError('unsupported measurement environment')
    if any(not isinstance(env.get(k), str) or not env[k] for k in ('hardware', 'os', 'architecture', 'toolchain')): raise ValueError('missing environment')
    runs = report.get('runs')
    if not isinstance(runs, list) or len(runs) != 20: raise ValueError('missing independent processes')
    for i, run in enumerate(runs): validate_run(run, i % 3, expanded=expanded)
    tables = []
    for index, size in enumerate(PROTOCOL['payloadBytes']):
        row = {'payloadBytes': size}
        for lane in LANES:
            values = [v for run in runs for v in run['scenarios'][index][lane]]
            row[lane] = {f'p{q}': performance.percentile(values, q/100) for q in (50, 95, 99)}
            row[lane]['samples'] = len(values)
        tables.append(row)
    result = {'status': 'diagnostic-only', 'tables': tables}
    if expanded:
        def distribution(values):
            return {**{f'p{q}': performance.percentile(values, q/100) for q in (50, 95, 99)}, 'samples': len(values)}
        operation_tables = []
        for index, size in enumerate(PROTOCOL['payloadBytes']):
            row = {'payloadBytes': size}
            for lane in OPERATION_LANES:
                samples = [run['operationScenarios'][index][lane] for run in runs]
                row[lane] = distribution([v for values in samples for v in values])
                # Retain independent-process summaries separately; pooled observations
                # and process p95s are different populations, neither is qualification.
                row[lane]['processMedian'] = distribution([performance.percentile(v, .5) for v in samples])
                row[lane]['processP95'] = distribution([performance.percentile(v, .95) for v in samples])
            operation_tables.append(row)
        result['operationTables'] = operation_tables
    return result

def evaluate_budget(report, budgets, revision, source_digest):
    performance.validate_budgets(budgets)
    limits=budgets.get('swift',{})
    if limits.get('protocolDigest')!=PROTOCOL_DIGEST:
        raise ValueError('approved current Swift fixture limits are required')
    for value,length in ((revision,40),(source_digest,64)):
        if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}',value):
            raise ValueError('expected candidate provenance is required')
    if report.get('dirty') is not False or report.get('revision')!=revision or report.get('sourceDigest')!=source_digest:
        raise ValueError('dirty or foreign Swift candidate')
    if report.get('schemaVersion')!=2 or report.get('kind')!='swift-diagnostic':
        raise ValueError('historical Swift diagnostics cannot qualify current budgets')
    summarize(report)  # Validate the full existing fixture, including durable reopen.
    rows=[];failures=[]
    for index,size in enumerate(PROTOCOL['payloadBytes']):
        for lane,limit in limits['metrics'].items():
            key='scenarios' if lane=='swiftTypedUs' else 'operationScenarios'
            samples=[run[key][index][lane] for run in report['runs']]
            pooled=performance.percentile([v for values in samples for v in values],.95)
            processes=performance.percentile([performance.percentile(values,.95) for values in samples],.95)
            rows.append({'payloadBytes':size,'metric':lane,'pooledP95':pooled,'processP95':processes,'limit':limit})
            if pooled>limit or processes>limit: failures.append(str(size)+' bytes '+lane+' exceeds budget')
    return {'status':'failed' if failures else 'passed','scope':limits['scope'],
            'hardwareQualified':False,'regressionQualified':False,
            'revision':revision,'sourceDigest':source_digest,'budgetDigest':performance.budget_digest(budgets),
            'failures':failures,'tables':rows}

def collect(prefix, report, output, timeout=600):
    report['complete'] = False
    for i in range(PROTOCOL['runs']):
        print(f'Swift diagnostic {i+1}/{PROTOCOL["runs"]}', flush=True)
        stdout = stderr = ''
        failure = None
        try:
            proc = subprocess.run([*prefix, '--order-offset', str(i % 3)], text=True, capture_output=True, timeout=timeout)
            stdout, stderr = proc.stdout, proc.stderr
            if proc.returncode: failure = {'kind': 'exit', 'exitCode': proc.returncode, 'error': stderr[-1000:]}
        except subprocess.TimeoutExpired as error:
            def text(value): return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
            stdout, stderr = text(error.stdout), text(error.stderr)
            failure = {'kind': 'timeout', 'error': f'timed out after {timeout} seconds'}
        except OSError as error:
            failure = {'kind': 'launch', 'error': str(error)}
        (output/f'run-{i}.stdout').write_text(stdout)
        (output/f'run-{i}.stderr').write_text(stderr)
        if failure is None:
            try:
                run = json.loads(stdout)
                validate_run(run, i % 3, expanded=True)
            except (ValueError, KeyError, TypeError) as error:
                failure = {'kind': 'invalid-output', 'error': str(error)}
        if failure:
            report['runFailures'].append({'run': i, **failure})
            return
        report['runs'].append(run)
    report['complete'] = True

def source_digest():
    # Include all package/build inputs and Swift wrapper sources, not just the engine.
    paths = [ROOT/'Package.swift', ROOT/'benchmarks/Package.swift', Path(__file__), ROOT/'tools/performance.py']
    for folder in ('cpp/src', 'cpp/include', 'cpp/c_api', 'cpp/third_party', 'swift/Sources', 'benchmarks/Sources/NovaSwiftOverhead'):
        paths.extend(p for p in (ROOT/folder).rglob('*') if p.is_file())
    digest = hashlib.sha256()
    for path in sorted(set(paths)): digest.update(str(path.relative_to(ROOT)).encode()+b'\0'+path.read_bytes()+b'\0')
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    command = performance.command
    report = {'schemaVersion': 2, 'kind': 'swift-diagnostic', 'complete': False, 'runFailures': [], 'runs': []}
    try:
        report.update({'schemaVersion': 2, 'kind': 'swift-diagnostic', 'revision': command(['git', 'rev-parse', 'HEAD'], cwd=ROOT), 'dirty': bool(command(['git', 'status', '--porcelain'], cwd=ROOT)), 'sourceDigest': source_digest(), 'protocol': PROTOCOL, 'protocolDigest': PROTOCOL_DIGEST, 'environment': {'platform': 'macos', 'hardware': command(['sysctl', '-n', 'hw.model']), 'os': platform.platform(), 'architecture': platform.machine(), 'toolchain': command(['xcodebuild', '-version'])+'\n'+command(['swift', '--version']), 'configuration': 'release'}, 'complete': False, 'runFailures': [], 'runs': []})
        build = ['swift', 'build', '--package-path', str(ROOT/'benchmarks'), '-c', 'release', '--product', 'NovaSwiftOverhead']
        with (out/'build.log').open('w') as log: subprocess.run(build, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=600)
        bin_path = command(['swift', 'build', '--package-path', ROOT/'benchmarks', '-c', 'release', '--show-bin-path'])
        collect([str(Path(bin_path)/'NovaSwiftOverhead')], report, out)
        # A changed source tree cannot be represented as a clean revision-bound run.
        if source_digest() != report['sourceDigest'] or command(['git', 'rev-parse', 'HEAD'], cwd=ROOT) != report['revision']:
            report['complete'] = False
            report['runFailures'].append({'kind': 'source-change', 'error': 'source or revision changed during measurement'})
        report['dirty'] = report['dirty'] or bool(command(['git', 'status', '--porcelain'], cwd=ROOT))
        if report['complete']: report['summary'] = summarize(report)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        report['complete'] = False
        report['runFailures'].append({'kind': 'collector', 'error': str(error)})
    finally:
        (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report.get('summary', report['runFailures']), indent=2))
    return 0 if report['complete'] else 1
if __name__ == '__main__': sys.exit(main())
