#!/usr/bin/env python3
"""Execute canonical examples; compare explicit expectations and recorded outputs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def command(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
    return result.stdout


def normalize(value):
    # Normalize protocol object timestamps and receipt transaction IDs only.
    # Graph properties, ordering, integer tags, LSNs, errors and plans remain exact.
    if isinstance(value, dict):
        value = {k: normalize(v) for k, v in value.items()}
        if value.get('kind') in ('node', 'edge') and 'properties' in value and 'version' in value:
            for key in ('last_read_ms', 'last_modified_ms'):
                if not isinstance(value.get(key), int) or value[key] < 0: raise AssertionError('invalid object timestamp')
                value[key] = '<timestamp-ms>'
        if value.get('schemaVersion') == 2 and value.get('ok') and 'receipt' in value:
            receipt = value['receipt']
            if not isinstance(receipt.get('transactionId'), str) or not receipt['transactionId']:
                raise AssertionError('missing transaction identity')
            receipt['transactionId'] = '<transaction-id>'
        return value
    if isinstance(value, list): return [normalize(v) for v in value]
    return value


def run_ngql(root, paths):
    with tempfile.TemporaryDirectory(prefix='nova-doc-example-') as directory:
        lines = command([str(root / 'build/cpp/nova-doc-example'), directory, *map(str, paths)]).splitlines()
        results = [normalize(json.loads(line)) for line in lines]
        if len(results) != len(paths) + 1 or results[-1] != {'schemaVersion': 2, 'ok': True, 'data': {}}:
            raise AssertionError('example close failed or response missing')
        if any(not item['ok'] for item in results[:-2]): raise AssertionError('example setup failed')
        return results[:-1]


def assert_output(actual, expected, label):
    if json.dumps(actual, sort_keys=True, allow_nan=False) != json.dumps(expected, sort_keys=True, allow_nan=False):
        raise AssertionError(f'{label}: output mismatch\nexpected: {json.dumps(expected, indent=2)}\nactual: {json.dumps(actual, indent=2)}')


def run_swift(root, work, code, sources=()):
    work.mkdir(parents=True, exist_ok=True)
    (work / 'Sources/DocExample').mkdir(parents=True, exist_ok=True)
    (work / 'Package.swift').write_text('// swift-tools-version: 6.0\nimport PackageDescription\nlet package = Package(name: "DocExample", platforms: [.macOS(.v13)], dependencies: [.package(name: "NovaGraph", path: '+json.dumps(str(root))+')], targets: [.executableTarget(name: "DocExample", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")])])\n')
    # @main files must not be named main.swift; top-level programs must be.
    target = work / 'Sources/DocExample'
    for old in target.glob('*.swift'): old.unlink()
    (target / ('Example.swift' if '@main' in code else 'main.swift')).write_text(code)
    for source in sources: (target / source.name).write_text(source.read_text())
    try: command(['swift', 'build'], cwd=work)
    except RuntimeError as error: raise RuntimeError(f'Swift compilation failed: {error}') from error
    binary = command(['swift', 'build', '--show-bin-path'], cwd=work).strip()
    return command([str(Path(binary) / 'DocExample')]).strip()


def execute(root, record=False, native_only=False):
    metadata = json.loads(command(['node', str(root / 'docs-web/scripts/example-metadata.mjs'), str(root)]))
    manifest = json.loads((root / 'examples/manifest.json').read_text())
    outputs = {}; checks = []
    groups = {}
    for entry in manifest['examples']:
        if entry['mode'] == 'fragment': continue
        if entry['language'] == 'ngql':
            paths = [root / p for p in entry['setup']] + [root / entry['path']]
            actual = run_ngql(root, paths)[-1]
            assert_output(actual, json.loads((root / entry['expected']).read_text()), entry['id'])
            outputs[entry['id']] = actual; checks.append(entry['id'])
        elif not native_only:
            groups.setdefault(entry['setupGroup'], []).append(entry)
    for group, entries in groups.items():
        language = entries[0]['language']
        if language == 'swift':
            code = '\n'.join((root / e['path']).read_text() for e in entries if e['mode'] == 'run')
            assertions = entries[0].get('assertions')
            if assertions: code += '\n' + (root / assertions).read_text()
            sources = [root / e['path'] for e in entries if e['mode'] == 'compile']
            output = run_swift(root, root / 'build/doc-swift' / group, code, sources)
            assert_output(output, entries[0]['expectedStdout'], group)
        elif language == 'cpp':
            for entry in entries:
                with tempfile.TemporaryDirectory(prefix='nova-doc-cpp-') as directory:
                    source = Path(directory) / 'main.cpp'; binary = Path(directory) / 'example'
                    source.write_text((root / entry['path']).read_text() + '\nint main() { ' + entry['entryPoint'] + '(); }\n')
                    command([os.environ.get('CXX', 'c++'), '-std=c++20', '-I'+str(root / 'cpp/include'), str(source), str(root / 'build/cpp/libnovagraphdb.a'), '-pthread', '-o', str(binary)])
                    command([str(binary)])
        checks.extend(e['id'] for e in entries)
    revision = command(['git', 'rev-parse', 'HEAD'], cwd=root).strip()
    result = {'schemaVersion': 1, 'revision': revision, **metadata, 'outputs': outputs}
    recorded = root / 'examples/recorded-results.json'
    if record: recorded.write_text(json.dumps(result, indent=2, ensure_ascii=False)+'\n')
    else:
        saved = json.loads(recorded.read_text())
        for key in ['engineDigest', 'inputDigest', 'outputs']: assert_output(result[key], saved[key], 'recorded '+key)
    evidence = root / 'build/docs-examples-evidence.json'; evidence.parent.mkdir(exist_ok=True)
    evidence.write_text(json.dumps({**result, 'dirty': bool(command(['git', 'status', '--porcelain'], cwd=root).strip()), 'platform': sys.platform, 'nativeOnly': native_only, 'checks': [{'id': 'docs.'+name, 'outcome': 'passed'} for name in checks]}, indent=2)+'\n')
    print(f'{len(checks)} executable documentation checks passed; actual NGQL outputs match expectations.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', action='store_true', help='replace recorded outputs only after assertions pass')
    parser.add_argument('--native-only', action='store_true', help='NGQL subset for Linux; macOS CI runs all examples')
    args = parser.parse_args()
    try: execute(Path(__file__).resolve().parents[1], args.record, args.native_only)
    except (RuntimeError, AssertionError, ValueError, OSError) as error:
        print(error, file=sys.stderr); sys.exit(1)
