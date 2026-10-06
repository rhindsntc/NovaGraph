#!/usr/bin/env python3
"""Fast controlled-gate contracts; no physical performance measurements."""
import argparse
import copy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import performance as p
import performance_gate as g


class GateContracts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.b = g.budgets()
        self.protocol = p.measurement_protocol(self.b, 'desktop', 'measure')
        self.protocol_hash = g.protocol_digest()
        self.environment = {'platform':'macos','hardware':'test','os':'test','toolchain':'test','architecture':g.platform.machine(),'compiler':'fixture clang','sdk':'fixture SDK','cpu':'fixture CPU','memoryBytes':1024**3}
        self.identity = {'revision':'a'*40, 'sourceDigest':'b'*64, 'protocolDigest':self.protocol_hash, 'dirty':False}

    def observation(self):
        def output(text, command):
            return {'command': command, 'stdout':text, 'stderr':'', 'returnCode':0}
        return {'timestamp':g.now(), 'power':output("Now drawing from 'AC Power'\n", ['pmset','-g','batt']),
                'thermal':output('No thermal warning level has been recorded\nNo CPU power warning level has been recorded\n',['pmset','-g','therm']),
                'processes':output('100 /usr/bin/python3\n',['ps','-A','-o','pid=,comm=']), 'loadAverage':[0.1,0.1,0.1], 'cpuCount':8}

    def report(self, label, value=100, revision=None):
        identity = dict(self.identity)
        if revision:
            identity['revision'] = revision
            identity['sourceDigest'] = 'c'*64
        directory = self.root / label
        directory.mkdir()
        binary = directory / 'nova-performance'
        binary.write_text('fixture native binary')
        log = directory / 'build.log'
        log.write_text('')
        prepared = {**identity, 'schemaVersion':1, 'kind':'nova-controlled-build', 'complete':True,
                    'failures':[], 'budgetDigest':p.budget_digest(self.b), 'protocol':self.protocol,
                    'environment':self.environment, 'buildFlags':g.BUILD_FLAGS,
                    'binary':g.file_record(binary), 'buildLog':g.file_record(log)}
        g.write_new(directory/'prepare.json', prepared)
        sequence = len(list(self.root.iterdir()))
        start = datetime(2026, 9, 28, tzinfo=timezone.utc) + timedelta(hours=sequence)
        control = {'schemaVersion':1, 'runnerId':'fixture-runner', 'operator':'fixture-operator', 'runId':label,
                   'exclusiveLock':str(g.LOCK_PATH), 'attestation':{'quiet':True, 'acPower':True, 'thermal':True},
                   'preflight':self.observation(), 'postflight':self.observation(), 'sourceStart':identity,
                   'sourceEnd':identity, 'startedAt':start.isoformat(), 'endedAt':(start+timedelta(minutes=30)).isoformat(),
                   'limitations':g.CONTROL_LIMITATIONS}
        report = {**identity, 'schemaVersion':1, 'profile':'desktop', 'complete':True, 'runFailures':[],
                  'environment':self.environment, 'protocol':self.protocol, 'budgetDigest':p.budget_digest(self.b),
                  'buildFlags':g.BUILD_FLAGS, 'prepared':g.file_record(directory/'prepare.json'),
                  'control':control, 'workloads':[]}
        for name in p.WORKLOADS:
            runs=[]
            for _ in range(self.protocol['runs']):
                runs.append({'platform':'macos', 'protocolVersion':2, 'mutationBatchSize':250,
                             'mutationValidation':{'singleCommits':120,'batchCommits':120,'mutations':30120,'reopenedRecords':250,'reopenVerified':True},
                             'mutationProtocol':{'records':250,'nodePropertyCount':1,'scalarType':'int64',
                                'operation':'upsert-existing-node','durability':'wal-sync','database':'dedicated',
                                'samples':self.protocol['samples'],'warmup':self.protocol['warmup'],
                                'validation':'each-commit-and-reopen'},
                             'dataset':{'name':name,'nodes':self.protocol['nodes'],'seed':self.protocol['seed'],'edges':10000,
                                'nodeStringPayloadBytes':1024 if name=='knowledge' else 512 if name=='messages' else 64,
                                'nodePropertyCount':2,'edgePropertyCount':0},
                             'metrics':{metric:[value]*(1 if metric in ('checkpointUs','openUs','trimUs') else self.protocol['samples']) for metric in p.METRICS},
                             'peakRSSBytes':1000})
            report['workloads'].append({'name':name,'runs':runs})
        return report

    def save(self, report):
        path=self.root/report['control']['runId']/'results.json'
        g.write_new(path, report)
        return path

    def freeze(self, first=None, second=None):
        first = first or self.report('first')
        second = second or self.report('second')
        output = self.root/'baseline.json'
        manifest=g.review(argparse.Namespace(reports=[self.save(first),self.save(second)],output=output,
                approve=True,reviewer='operator',approval_note='Reviewed every retained run and repeatability'))
        return output,manifest

    def run_gate(self, candidate, baseline):
        return g.gate(argparse.Namespace(candidate=self.save(candidate), baseline=baseline, output=self.root/'gate.json'))

    def test_complete_review_and_gate_allows_engine_revision_change(self):
        baseline, manifest=self.freeze()
        candidate=self.report('candidate',110,revision='d'*40)
        result=self.run_gate(candidate,baseline)
        self.assertEqual(result['status'],'passed',result)
        self.assertEqual(result['absoluteAcceptance'],'passed')
        self.assertEqual(result['regressionAcceptance'],'passed')
        self.assertEqual(len(result['comparisons']),2)
        self.assertFalse(manifest['approval']['externalHumanIdentityVerified'])

    def test_above_ten_percent_fails_and_exact_boundary_passes(self):
        baseline,_=self.freeze()
        result=self.run_gate(self.report('candidate',110.00001),baseline)
        self.assertEqual(result['status'],'failed')
        self.assertEqual(result['absoluteAcceptance'],'passed')
        self.assertEqual(result['regressionAcceptance'],'failed')
        self.assertEqual(len(result['comparisons']),2)

    def test_repeatability_checks_both_directions_and_requires_same_revision(self):
        first=self.report('first',100)
        second=self.report('second',89)
        with self.assertRaisesRegex(ValueError,'repeatability'):g.validate_pair([first,second],self.b)
        second=self.report('third',100,revision='d'*40)
        with self.assertRaisesRegex(ValueError,'same frozen'):g.validate_pair([first,second],self.b)

    def test_pair_exact_ten_percent_passes(self):
        g.validate_pair([self.report('first',100),self.report('second',110)],self.b)

    def test_replay_and_overlapping_runs_are_rejected(self):
        first=self.report('first')
        with self.assertRaisesRegex(ValueError,'independent'):g.validate_pair([first,copy.deepcopy(first)],self.b)
        second=self.report('second')
        second['control']['startedAt']=first['control']['startedAt']
        with self.assertRaisesRegex(ValueError,'overlap'):g.validate_pair([first,second],self.b)

    def test_no_baseline_never_passes_regression(self):
        result=self.run_gate(self.report('candidate'),None)
        self.assertEqual(result['status'],'failed')
        self.assertEqual(result['absoluteAcceptance'],'passed')
        self.assertEqual(result['regressionAcceptance'],'not-evaluated')
        self.assertIn('requires',result['failures'][0])

    def test_invalid_report_provenance_and_control_is_rejected(self):
        report=self.report('first')
        mutations=[('dirty',True),('complete',False),('revision','bad'),('budgetDigest','f'*64),
                   ('protocolDigest','f'*64),('buildFlags',['-O0'])]
        for key,value in mutations:
            changed=copy.deepcopy(report);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):g.validate_report(changed,self.b)
        for change in ('dirty_end','no_attestation','bad_thermal','battery','busy','compiler','smoke','simulator','missing_metric','nan','absolute_failure'):
            changed=copy.deepcopy(report)
            if change=='dirty_end':changed['control']['sourceEnd']['dirty']=True
            elif change=='no_attestation':changed['control']['attestation']['quiet']=False
            elif change=='bad_thermal':changed['control']['postflight']['thermal']['stdout']='Thermal_Level = 1'
            elif change=='battery':changed['control']['preflight']['power']['stdout']="Now drawing from 'Battery Power'"
            elif change=='busy':changed['control']['preflight']['loadAverage'][0]=40
            elif change=='compiler':changed['control']['preflight']['processes']['stdout']='30 /usr/bin/clang++\n'
            elif change=='smoke':changed['protocol']['mode']='smoke'
            elif change=='simulator':changed['environment']['platform']='ios-simulator'
            elif change=='missing_metric':del changed['workloads'][0]['runs'][0]['metrics']['hotReadUs']
            elif change=='nan':changed['workloads'][0]['runs'][0]['metrics']['hotReadUs'][0]=float('nan')
            elif change=='absolute_failure':changed['workloads'][0]['runs'][0]['peakRSSBytes']=10**12
            with self.subTest(change=change),self.assertRaises((ValueError,KeyError)):g.validate_report(changed,self.b)

    def test_environment_and_runner_differences_reject_gate(self):
        baseline,_=self.freeze()
        candidate=self.report('candidate')
        candidate['control']['runnerId']='another-runner'
        result=self.run_gate(candidate,baseline)
        self.assertEqual(result['status'],'failed')
        self.assertIn('differs',result['failures'][0])

    def test_tampered_report_and_manifest_rejected(self):
        baseline,manifest=self.freeze()
        report_path=Path(manifest['reports'][0]['path'])
        original=report_path.read_text()
        report_path.write_text(original+' ')
        with self.assertRaisesRegex(ValueError,'hash/size'):g.load_baseline(baseline,self.b)
        report_path.write_text(original)
        manifest['approval']['note']='edited'
        baseline.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError,'manifest hash'):g.load_baseline(baseline,self.b)

    def test_absolute_failed_baseline_rejected_even_with_passed_label(self):
        first=self.report('first')
        first['evaluation']={'status':'passed','hardwareQualified':True}
        first['workloads'][0]['runs'][0]['peakRSSBytes']=10**12
        with self.assertRaisesRegex(ValueError,'absolute'):self.freeze(first,self.report('second'))
        self.assertFalse((self.root/'baseline.json').exists())

    def test_binary_and_manifest_tampering_are_rejected(self):
        report=self.report('first')
        (self.root/'first'/'nova-performance').write_text('changed')
        with self.assertRaisesRegex(ValueError,'hash/size'):g.validate_report(report,self.b)

    def test_explicit_approval_required_and_evidence_never_overwritten(self):
        with self.assertRaisesRegex(ValueError,'explicit'):g.review(argparse.Namespace(approve=False,reviewer='x',approval_note='x'))
        path=self.root/'existing.json';g.write_new(path,{'original':True})
        with self.assertRaises(FileExistsError):g.write_new(path,{'original':False})
        self.assertEqual(g.read_json(path),{'original':True})

    def test_real_subprocess_machine_lock_contention(self):
        script=('import sys;sys.path.insert(0,sys.argv[1]);import performance_gate as g\n'
                'try:\n with g.measurement_lock():print("acquired")\n'
                'except ValueError as e:print(str(e));sys.exit(7)\n')
        with g.measurement_lock():
            proc=subprocess.run([sys.executable,'-c',script,str(Path(g.__file__).parent)],text=True,capture_output=True,timeout=10)
            self.assertEqual(proc.returncode,7,proc.stderr)
            self.assertIn('busy',proc.stdout)
        proc=subprocess.run([sys.executable,'-c',script,str(Path(g.__file__).parent)],text=True,capture_output=True,timeout=10)
        self.assertEqual(proc.returncode,0,proc.stderr)
        self.assertEqual(proc.stdout.strip(),'acquired')

    def test_prepare_rejects_dirty_and_preserves_failure(self):
        args=argparse.Namespace(output=self.root/'prepare')
        identity={**self.identity,'dirty':True}
        with patch.object(g,'source_identity',return_value=identity):
            result=g.prepare(args)
        self.assertFalse(result['complete'])
        self.assertIn('clean',result['failures'][0])
        self.assertTrue((args.output/'prepare.json').exists())

    def test_ordinary_collector_rejected_before_setup_by_gate_lock(self):
        output=self.root/'ordinary'
        script=('import sys;sys.path.insert(0,sys.argv[1]);import performance as p\n'
                'from pathlib import Path\nimport argparse\n'
                'def forbidden(*args, **kwargs):raise RuntimeError("ordinary build was reached")\n'
                'p.build=forbidden\np.command=lambda *a,**k:"fixture"\np.source_digest=lambda:"b"*64\n'
                'p.run(argparse.Namespace(mode="smoke",baseline=None,profile="desktop",platform="macos",output=Path(sys.argv[2])))\n')
        with g.measurement_lock():
            proc=subprocess.run([sys.executable,'-c',script,str(Path(p.__file__).parent),str(output)],
                                text=True,capture_output=True,timeout=10)
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('lock is busy',proc.stderr)
        self.assertFalse(output.exists(), 'the normal collector must be blocked before build/setup')

    def test_prepare_cannot_compile_during_a_measurement_window(self):
        args=argparse.Namespace(output=self.root/'prepare')
        with g.measurement_lock(),patch.object(g,'source_identity',return_value=self.identity), \
             patch.object(g,'environment',return_value=self.environment):
            result=g.prepare(args)
        self.assertFalse(result['complete'])
        self.assertIn('lock is busy',result['failures'][0])
        self.assertFalse((args.output/'nova-performance').exists())

    def test_build_stale_identity_is_rejected(self):
        report=self.report('first');prepared=g.read_json(report['prepared']['path'])
        with patch.object(g,'source_identity',return_value={**self.identity,'sourceDigest':'d'*64}):
            with self.assertRaisesRegex(ValueError,'stale'):g.validate_prepared(prepared,self.b,live=True)

    def test_candidate_compares_against_both_not_fastest_or_slowest_selected(self):
        baseline,_=self.freeze(self.report('first',110),self.report('second',100))
        result=self.run_gate(self.report('candidate',121),baseline)
        self.assertEqual(result['status'],'failed')
        self.assertEqual([entry['evaluation']['status'] for entry in result['comparisons']],['passed','failed'])

    def test_collect_preserves_preflight_failure_and_never_builds(self):
        fixture=self.report('fixture')
        bad=self.observation();bad['power']['stdout']="Now drawing from 'Battery Power'"
        args=argparse.Namespace(prepared=self.root/'fixture',output=self.root/'collection',runner_id='fixture',
             operator='operator',attest_quiet=True,attest_ac_power=True,attest_thermal=True)
        with patch.object(g,'source_identity',return_value=self.identity), \
             patch.object(g,'environment',return_value=self.environment),patch.object(g,'observe',return_value=bad):
            result=g.collect(args)
        self.assertFalse(result['complete'])
        self.assertIn('AC power',result['runFailures'][0]['error'])
        self.assertEqual(result['workloads'],[])
        self.assertTrue((args.output/'results.json').exists())
        self.assertEqual(result['control']['postflight'],bad)

    def test_collect_runs_prebuilt_fixture_and_detects_dirty_end(self):
        fixture=self.report('fixture')
        prepared_path=self.root/'fixture'/'prepare.json'
        prepared=g.read_json(prepared_path)
        worker=self.root/'fixture'/'nova-performance'
        worker.write_text('#!'+sys.executable+'\nimport json,sys\nfrom pathlib import Path\n'
           'data=json.loads(Path(__file__).with_name("runs.json").read_text())\nprint(json.dumps(data[sys.argv[1]]))\n')
        worker.chmod(0o755)
        g.write_new(worker.with_name('runs.json'),{work['name']:work['runs'][0] for work in fixture['workloads']})
        prepared['binary']=g.file_record(worker);prepared_path.write_text(json.dumps(prepared))
        args=argparse.Namespace(prepared=worker.parent,output=self.root/'collection',runner_id='fixture',
             operator='operator',attest_quiet=True,attest_ac_power=True,attest_thermal=True)
        # Four start/build checks precede the postflight source check. No build function is replaced.
        identities=[self.identity,self.identity,self.identity,{**self.identity,'dirty':True}]
        with patch.object(g,'source_identity',side_effect=identities), \
             patch.object(g,'environment',return_value=self.environment),patch.object(g,'observe',side_effect=[self.observation(),self.observation()]):
            result=g.collect(args)
        self.assertFalse(result['complete'])
        self.assertIn('clean',result['runFailures'][0]['error'])
        self.assertTrue((args.output/'contacts-0.stdout').exists())
        self.assertTrue((args.output/'recommendations-19.stdout').exists())
        self.assertEqual(len(result['workloads']),5)

    def test_protocol_digest_excludes_engine_but_tracks_collector(self):
        root=self.root/'repo';(root/'tools').mkdir(parents=True);(root/'benchmarks/performance').mkdir(parents=True)
        for name in ('performance.py','performance_gate.py','performance_lock.py','resource_measurements.py','performance_activity.py','performance_diagnostic.py'):(root/'tools'/name).write_text(name)
        (root/'benchmarks/budgets.json').write_text('{}')
        (root/'benchmarks/performance/native.cpp').write_text('native')
        (root/'cpp').mkdir();engine=root/'cpp/engine.cpp';engine.write_text('before')
        with patch.object(g,'ROOT',root):
            before=g.protocol_digest();engine.write_text('after');self.assertEqual(before,g.protocol_digest())
            (root/'tools/performance.py').write_text('changed');self.assertNotEqual(before,g.protocol_digest())
            before=g.protocol_digest();(root/'tools/performance_lock.py').write_text('lock changed');self.assertNotEqual(before,g.protocol_digest())
            for module in ('performance_activity.py','performance_diagnostic.py'):
                before=g.protocol_digest();(root/'tools'/module).write_text('diagnostic changed');self.assertNotEqual(before,g.protocol_digest())

    def test_incomplete_observation_or_hardware_identity_is_rejected(self):
        report=self.report('first')
        prepared=g.read_json(report['prepared']['path']);del prepared['environment']['compiler']
        with self.assertRaisesRegex(ValueError,'environment'):g.validate_prepared(prepared,self.b)
        observation=self.observation();observation['power']['command']=['fake']
        self.assertTrue(g.observation_failures(observation))

    def test_headless_simulator_process_blocks_collection(self):
        observation=self.observation();observation['processes']['stdout']='300 /usr/libexec/launchd_sim\n'
        self.assertTrue(g.observation_failures(observation))

    def test_thermal_cpu_throttling_and_unavailable_readings_rejected(self):
        observation=self.observation();observation['thermal']['stdout']+='CPU_Speed_Limit = 80\n'
        self.assertTrue(g.observation_failures(observation))
        observation=self.observation();observation['power']['returnCode']=1
        self.assertTrue(g.observation_failures(observation))

if __name__=='__main__':
    unittest.main()
