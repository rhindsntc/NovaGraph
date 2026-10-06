#!/usr/bin/env python3
"""Mutation acceptance checks run against the real compiler and engine."""
import importlib.util
import tempfile
import shutil
import unittest
from pathlib import Path
spec = importlib.util.spec_from_file_location('examples', Path(__file__).with_name('test-doc-examples.py'))
examples = importlib.util.module_from_spec(spec)
spec.loader.exec_module(examples)
ROOT = Path(__file__).resolve().parents[1]
class Contracts(unittest.TestCase):
    def test_wrong_graph_value_and_error_code_fail_parity(self):
        with tempfile.TemporaryDirectory() as directory:
            query = Path(directory) / 'query.ngql'
            query.write_text('upsert node Person ada set name="Ada"; get node ada;')
            result = examples.run_ngql(ROOT, [query])[-1]
            self.assertEqual(result['ok'], True)
            self.assertIn('Ada', str(result))
            with self.assertRaisesRegex(AssertionError, 'output mismatch'):
                examples.assert_output(result, {'ok': True, 'data': {'name': 'Grace'}}, 'mutation')
            query.write_text('find nodes Person limit -1;')
            failure = examples.run_ngql(ROOT, [query])[-1]
            self.assertEqual(failure['error']['code'], 'parseError')
            with self.assertRaisesRegex(AssertionError, 'output mismatch'):
                examples.assert_output(failure, {'ok': False, 'error': {'code': 'cancelled'}}, 'mutation')
    @unittest.skipUnless(__import__('sys').platform == 'darwin', 'Swift public package is checked on macOS')
    def test_changed_public_api_call_fails_compilation(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory).resolve()
            shutil.copytree(ROOT / 'swift', fixture / 'swift', ignore=shutil.ignore_patterns('.build', '.swiftpm'))
            shutil.copy2(ROOT / 'Package.swift', fixture / 'Package.swift')
            shutil.copytree(ROOT / 'cpp', fixture / 'cpp')
            code = 'import Foundation\nimport GraphDBKit\nlet path = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)\ndefer { try? FileManager.default.removeItem(at: path) }\nlet db = try GraphDatabase(path: path)\ntry db.close()\n'
            examples.run_swift(fixture, fixture / 'consumer', code)
            api = fixture / 'swift/Sources/GraphDBKit/GraphDB.swift'
            original = api.read_text()
            changed = original.replace('convenience init(\n        path: URL', 'convenience init(\n        changed path: URL')
            self.assertTrue(changed != original, 'API mutation did not apply')
            api.write_text(changed)
            with self.assertRaisesRegex(RuntimeError, 'Swift compilation failed'):
                examples.run_swift(fixture, fixture / 'consumer', code)
    def test_scalar_kind_changes_cannot_pass_output_comparison(self):
        with self.assertRaisesRegex(AssertionError, 'output mismatch'):
            examples.assert_output({'value': True}, {'value': 1}, 'scalar mutation')
    def test_every_cpp_source_in_a_group_must_compile(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory).resolve()
            (fixture / 'examples').mkdir()
            (fixture / 'tools').mkdir()
            (fixture / 'docs-web/scripts').mkdir(parents=True)
            (fixture / 'build/cpp').mkdir(parents=True)
            shutil.copytree(ROOT / 'cpp/include', fixture / 'cpp/include')
            shutil.copy(ROOT / 'docs-web/scripts/example-metadata.mjs', fixture / 'docs-web/scripts')
            (fixture / 'build/cpp/libnovagraphdb.a').symlink_to(ROOT / 'build/cpp/libnovagraphdb.a')
            (fixture / 'examples/good.cpp').write_text('void value_fidelity_example() {}\n')
            (fixture / 'examples/bad.cpp').write_text('#error THIS EXAMPLE MUST NOT COMPILE\n')
            entries = [dict(id=name, path=f'examples/{name}.cpp', language='cpp', mode='run', setupGroup='both', setup=[], platforms=['macOS','Linux'], entryPoint='value_fidelity_example') for name in ['good','bad']]
            (fixture / 'examples/manifest.json').write_text(json.dumps(dict(schemaVersion=1,examples=entries)))
            examples.command(['git','init','-q'], cwd=fixture)
            examples.command(['git','-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','--allow-empty','-qm','fixture'], cwd=fixture)
            with self.assertRaisesRegex(RuntimeError, 'THIS EXAMPLE MUST NOT COMPILE'):
                examples.execute(fixture, record=True)
    @unittest.skipUnless(__import__('sys').platform == 'darwin', 'Swift public package is checked on macOS')
    def test_parameter_query_cannot_return_empty_and_pass_its_example(self):
        import json
        entries = json.loads((ROOT / 'examples/manifest.json').read_text())['examples']
        guide = [e for e in entries if e['setupGroup'] == 'swift-guide']
        parts = []
        for entry in guide:
            code = (ROOT / entry['path']).read_text()
            if entry['id'] == 'swift-parameters': code = code.replace('limit 1', 'limit 0')
            parts.append(code)
        code = '\n'.join(parts) + '\n' + (ROOT / guide[0]['assertions']).read_text()
        with tempfile.TemporaryDirectory() as directory:
            # Failed preconditions terminate Swift before defer runs; keep graph data
            # under the Python fixture so even the intentional trap is cleaned up.
            isolated = code.replace('FileManager.default.temporaryDirectory', 'URL(fileURLWithPath: ' + json.dumps(directory) + ')')
            with self.assertRaisesRegex(RuntimeError, 'failed') as caught:
                examples.run_swift(ROOT, Path(directory), isolated)
            self.assertNotIn('Swift compilation failed', str(caught.exception))

if __name__ == '__main__': unittest.main()
