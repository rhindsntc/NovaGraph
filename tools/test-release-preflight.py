#!/usr/bin/env python3
"""Exercise release preflight against real disposable Git trees and file evidence."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/release_preflight.py'

class ReleasePreflightContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='nova-release-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo = self.base / 'repo'; self.repo.mkdir()
        archive = self.base / 'source.tar'
        archive.write_bytes(subprocess.check_output(['git', 'archive', 'HEAD'], cwd=ROOT))
        with tarfile.open(archive) as source: source.extractall(self.repo, filter='data')
        (self.repo / 'docs-web/node_modules').symlink_to(ROOT / 'docs-web/node_modules', target_is_directory=True)
        subprocess.run(['git','init','-q'],cwd=self.repo,check=True,capture_output=True)
        (self.repo/'.git/info/exclude').write_text('docs-web/node_modules\n')
        for command in (['git','add','.'],
                        ['git','-c','user.name=Release Fixture','-c','user.email=fixture@example.invalid','commit','-qm','fixture']):
            subprocess.run(command,cwd=self.repo,check=True,capture_output=True)
        self.revision = subprocess.check_output(['git','rev-parse','HEAD'],cwd=self.repo,text=True).strip()
        self.inputs = self.base / 'inputs'; self.inputs.mkdir()
        self.entries = []
    def receipt(self, gate='native-debug'):
        catalog = json.loads((self.repo/'tests/catalog.json').read_text())['tests']
        ids = [t['id'] for t in catalog if t['language']=='cpp']
        raw = self.inputs / 'native.log'; raw.write_text('trusted test producer fixture\n')
        record = dict(schemaVersion=1,gate=gate,revision=self.revision,dirty=False,
            outcome='passed',checks=[dict(id=i,outcome='passed') for i in ids],
            artifacts=[dict(path=raw.name,sha256=hashlib.sha256(raw.read_bytes()).hexdigest())])
        self.save_receipt(record)
        return record
    def save_receipt(self, record):
        path = self.inputs / (record['gate']+'.json'); path.write_text(json.dumps(record))
        self.entries = [x for x in self.entries if x['gate']!=record['gate']]
        self.entries.append(dict(gate=record['gate'],path=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    def run_preflight(self, output='out', extra=()):
        index=self.inputs/'index.json';index.write_text(json.dumps(dict(schemaVersion=1,receipts=self.entries)))
        run=subprocess.run(['python3',str(SCRIPT),'--root',str(self.repo),'--version','0.1.0',
            '--evidence-index',str(index),'--output',str(self.base/output),*extra],capture_output=True,text=True,timeout=60)
        path=self.base/output/'report.json'
        self.assertTrue(path.exists(),run.stderr)
        return run,json.loads(path.read_text())
    def gate(self, report, name):
        return next(g for g in report['gates'] if g['id']==name)
    def test_missing_evidence_is_explicit_no_go(self):
        run,report=self.run_preflight()
        self.assertEqual(run.returncode,1);self.assertEqual(report['decision'],'no-go')
        self.assertFalse(report['productionQualified'])
        self.assertEqual(self.gate(report,'documentation-parity')['status'],'passed')
        self.assertEqual(self.gate(report,'quality-qualification')['status'],'pending')
        self.assertEqual(self.gate(report,'apple-artifacts')['status'],'missing')
        manifest=json.loads((self.base/'out/candidate-manifest.json').read_text())
        self.assertEqual(manifest['revision'],self.revision)
        self.assertEqual(manifest['version'],'0.1.0')
        self.assertEqual(manifest['format']['wal'],4)
        self.assertEqual(manifest['format']['rootCatalogRecord'],3)
        self.assertRegex(manifest['documentation']['sourceDigest'],r'^[0-9a-f]{64}$')
    def test_complete_native_receipt_retains_hashes_without_certifying_release(self):
        self.receipt();run,report=self.run_preflight()
        self.assertEqual(run.returncode,1)
        self.assertEqual(self.gate(report,'native-debug')['status'],'accepted')
        self.assertFalse(report['productionQualified'])
        self.assertEqual(self.gate(report,'native-release')['status'],'missing')
    def test_foreign_revision_cannot_supply_candidate_evidence(self):
        record=self.receipt();record['revision']='a'*40;self.save_receipt(record)
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_skipped_required_check_blocks_release(self):
        record=self.receipt();record['checks'][0]['outcome']='skipped';self.save_receipt(record)
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_omitted_required_check_cannot_be_hidden_by_passing_summary(self):
        record=self.receipt();record['checks'].pop();self.save_receipt(record)
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_changed_raw_evidence_is_rejected(self):
        self.receipt();(self.inputs/'native.log').write_text('changed after recording\n')
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_stale_documentation_hash_blocks_release(self):
        p=self.repo/'docs-web/content/overview.md';p.write_text(p.read_text()+'\nUpdated guide.\n')
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'documentation-parity')['status'],'rejected')
    def test_changed_example_blocks_documentation_parity(self):
        p=self.repo/'examples/ngql/ngql-basics.ngql';p.write_text(p.read_text()+'\n# changed input\n')
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'documentation-parity')['status'],'rejected')
    def test_missing_watch_slice_blocks_artifact_gate(self):
        import sys
        sys.path.insert(0,str(ROOT/'tools'))
        import apple_artifacts
        bundle=self.base/'apple';bundle.mkdir()
        (bundle/'artifact-manifest.json').write_text(json.dumps(dict(schemaVersion=1,
            slices=[x for x in apple_artifacts.SLICES if x['id']!='watchos'])))
        _,report=self.run_preflight(extra=['--apple-bundle',str(bundle)])
        gate=self.gate(report,'apple-artifacts')
        self.assertEqual(gate['status'],'rejected');self.assertIn('matrix',gate['reason'])
    def test_existing_report_is_never_overwritten(self):
        self.run_preflight();path=self.base/'out/report.json';original=path.read_bytes()
        run,_=self.run_preflight();self.assertNotEqual(run.returncode,0);self.assertEqual(path.read_bytes(),original)
    def test_evidence_path_cannot_escape_its_index_directory(self):
        record=self.receipt();record['artifacts'][0]['path']='../source.tar';self.save_receipt(record)
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_candidate_identity_is_captured_before_validator_side_effects(self):
        script=self.repo/'docs-web/scripts/generate-docs.mjs'
        script.write_text(script.read_text()+"\nawait writeFile(fileURLToPath(new URL('../../README.md', import.meta.url)), 'changed during validation\\n');\n")
        subprocess.run(['git','add','.'],cwd=self.repo,check=True,capture_output=True)
        subprocess.run(['git','-c','user.name=Release Fixture','-c','user.email=fixture@example.invalid','commit','-qm','validator fixture'],cwd=self.repo,check=True,capture_output=True)
        _,report=self.run_preflight()
        self.assertEqual(self.gate(report,'candidate-source')['status'],'passed')
        self.assertEqual(self.gate(report,'source-stability')['status'],'rejected')
        manifest=json.loads((self.base/'out/candidate-manifest.json').read_text())
        self.assertFalse(manifest['dirty'])
        self.assertTrue(self.gate(report,'source-stability')['observed']['dirty'])
    def test_deferred_quality_gate_cannot_be_waived_by_an_input_receipt(self):
        self.entries=[dict(gate='quality-qualification',path='not-used.json',sha256='a'*64)]
        _,report=self.run_preflight()
        self.assertEqual(self.gate(report,'evidence-index')['status'],'rejected')
        self.assertEqual(self.gate(report,'quality-qualification')['status'],'pending')
        self.assertFalse(report['productionQualified'])
    def test_changed_receipt_is_rejected_even_when_raw_log_is_unchanged(self):
        self.receipt();p=self.inputs/'native-debug.json';p.write_text(p.read_text()+'\n')
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_non_object_receipt_retains_rejected_gate(self):
        for value in ([], None, "invalid", True):
            with self.subTest(value=value):
                self.receipt()
                path=self.inputs/'native-debug.json';path.write_text(json.dumps(value))
                self.entries[0]['sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
                run,report=self.run_preflight(output='receipt-'+str(type(value).__name__))
                self.assertEqual(run.returncode,1);self.assertNotIn('Traceback',run.stderr)
                self.assertEqual(self.gate(report,'native-debug')['status'],'rejected')
    def test_non_object_index_retains_rejected_gate(self):
        for value in ([], None, "invalid", True):
            with self.subTest(value=value):
                path=self.inputs/'malformed-index.json';path.write_text(json.dumps(value))
                run,report=self.run_preflight(output='index-'+str(type(value).__name__),extra=['--evidence-index',str(path)])
                self.assertEqual(run.returncode,1);self.assertNotIn('Traceback',run.stderr)
                self.assertEqual(self.gate(report,'evidence-index')['status'],'rejected')
    def test_non_object_apple_manifest_retains_rejected_gate(self):
        bundle=self.base/'apple';bundle.mkdir()
        (bundle/'artifact-manifest.json').write_text('[]')
        run,report=self.run_preflight(extra=['--apple-bundle',str(bundle)])
        self.assertEqual(run.returncode,1);self.assertNotIn('Traceback',run.stderr)
        self.assertEqual(self.gate(report,'apple-artifacts')['status'],'rejected')
    def test_dirty_candidate_is_no_go(self):
        (self.repo/'README.md').write_text('uncommitted change\n')
        _,report=self.run_preflight();self.assertEqual(self.gate(report,'candidate-source')['status'],'rejected')

if __name__=='__main__':unittest.main()
