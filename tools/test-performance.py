#!/usr/bin/env python3
import copy
import argparse
import contextlib
import io
import json
import sys
import tempfile
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
from resource_measurements import mutation_protocol
spec=importlib.util.spec_from_file_location('performance',Path(__file__).with_name('performance.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
class PerformanceContracts(unittest.TestCase):
    def setUp(self):
        self.budgets=json.loads((p.ROOT/'benchmarks/budgets.json').read_text())
        self.report={'revision':'a'*40,'sourceDigest':'b'*64,'dirty':False,'runFailures':[],'complete':True,'schemaVersion':1,'profile':'desktop','environment':{'platform':'macos','hardware':'test','os':'test','toolchain':'test','architecture':'arm64'},'protocol':{'version':2,'mutationBatchSize':250,'seed':42,'nodes':10000,'warmup':20,'samples':100,'runs':20,'mode':'measure'},'workloads':[]}
        for name in ('contacts','messages','knowledge','navigation','recommendations'):
            self.report['workloads'].append({'name':name,'runs':[{'protocolVersion':2,'mutationBatchSize':250,'mutationProtocol':mutation_protocol(),'mutationValidation':{'singleCommits':120,'batchCommits':120,'mutations':30120,'reopenedRecords':250,'reopenVerified':True},'platform':'macos','dataset':{'name':name,'nodes':10000,'seed':42,'edges':10000,'nodeStringPayloadBytes':1024 if name=='knowledge' else 512 if name=='messages' else 64,'nodePropertyCount':2,'edgePropertyCount':0},'metrics':{'hotReadUs':[1]*100,'coldReadUs':[2]*100,'checkpointUs':[3],'openUs':[4],'trimUs':[5],'novaTraversalUs':[6]*100,'singleMutationUs':[7]*100,'batchMutationUs':[8]*100},'peakRSSBytes':1000} for _ in range(20)]})
    def test_failed_or_dirty_baseline_is_not_qualified(self):
        self.report['complete']=False
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();self.report['dirty']=True
        self.assertEqual(p.evaluate(self.report,self.budgets)['status'],'unqualified')
        baseline=copy.deepcopy(self.report);self.report['dirty']=False
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets,baseline)
    def test_absolute_failed_baseline_is_rejected_before_comparison(self):
        baseline=copy.deepcopy(self.report)
        for run in baseline['workloads'][0]['runs']:run['metrics']['hotReadUs']=[3000]*100
        with self.assertRaisesRegex(ValueError,'baseline'):
            p.evaluate(self.report,self.budgets,baseline)

    def test_independent_process_tail_regressions_fail(self):
        for metric in ('trimUs','checkpointUs','openUs','hotReadUs'):
            self.setUp();baseline=copy.deepcopy(self.report)
            for run in self.report['workloads'][0]['runs'][-2:]:
                run['metrics'][metric]=[1000]*len(run['metrics'][metric])
            self.assertEqual(p.evaluate(self.report,self.budgets,baseline)['status'],'failed')

    def test_unapproved_or_invalid_budgets_fail_closed(self):
        for key,value in [('schemaVersion',1),('units',{}),('review',{'status':'proposed'})]:
            budgets=copy.deepcopy(self.budgets);budgets[key]=value
            with self.assertRaises(ValueError):p.evaluate(self.report,budgets)
        for value in (False,0,-1,float('inf'),float('nan')):
            budgets=copy.deepcopy(self.budgets);budgets['classes']['desktop']['trimUs']=value
            with self.assertRaises(ValueError):p.evaluate(self.report,budgets)

    def test_new_operation_limits_have_exact_boundaries(self):
        for metric in ('trimUs','novaTraversalUs','singleMutationUs','batchMutationUs'):
            report=copy.deepcopy(self.report);limit=self.budgets['classes']['desktop'][metric]
            for run in report['workloads'][0]['runs']:
                run['metrics'][metric]=[limit]*len(run['metrics'][metric])
            self.assertEqual(p.evaluate(report,self.budgets)['status'],'passed')
            for run in report['workloads'][0]['runs']:
                run['metrics'][metric]=[limit+1]*len(run['metrics'][metric])
            self.assertEqual(p.evaluate(report,self.budgets)['status'],'failed')

    def test_missing_protocol_or_mutation_samples_cannot_qualify(self):
        for field in ('version','mutationBatchSize'):
            report=copy.deepcopy(self.report);del report['protocol'][field]
            with self.assertRaises(ValueError):p.evaluate(report,self.budgets)
        for field in ('protocolVersion','mutationBatchSize'):
            report=copy.deepcopy(self.report);del report['workloads'][0]['runs'][0][field]
            with self.assertRaises(ValueError):p.evaluate(report,self.budgets)
        for metric in ('trimUs','novaTraversalUs','singleMutationUs','batchMutationUs'):
            report=copy.deepcopy(self.report);del report['workloads'][0]['runs'][0]['metrics'][metric]
            with self.assertRaises(ValueError):p.evaluate(report,self.budgets)
        report=copy.deepcopy(self.report);report['workloads'][0]['runs'][0]['metrics']['batchMutationUs']=[1]
        with self.assertRaises(ValueError):p.evaluate(report,self.budgets)

    def test_conflicting_run_metadata_is_rejected(self):
        for field,value in [('nodes',32),('seed',99),('name','other'),('nodeStringPayloadBytes',0)]:
            self.setUp();self.report['workloads'][0]['runs'][0]['dataset'][field]=value
            with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();self.report['workloads'][0]['runs'][0]['platform']='ios-simulator'
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();baseline=copy.deepcopy(self.report)
        for r in baseline['workloads'][0]['runs']:r['dataset']['edges']=1234
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets,baseline)
    def test_missing_provenance_and_recorded_failure_are_rejected(self):
        for field in ('revision','sourceDigest','dirty','runFailures'):
            self.setUp();del self.report[field]
            with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        for field,value in [('revision','bad'),('sourceDigest','bad'),('dirty',0),('runFailures',None)]:
            self.setUp();self.report[field]=value
            with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();self.report['runFailures']=[{'error':'timeout'}]
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();baseline=copy.deepcopy(self.report);del baseline['revision']
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets,baseline)
    def test_timeout_retains_output_and_continues_other_workloads(self):
        with tempfile.TemporaryDirectory() as path:
            out=Path(path);script=out/'worker.py'
            script.write_text("import sys,time,json\nif sys.argv[1]=='contacts':\n print('partial output',flush=True);print('partial error',file=sys.stderr,flush=True);time.sleep(2)\nelse: print(json.dumps({'platform':'macos','dataset':{'nodes':10000}}))\n")
            self.report['protocol']['runs']=1;self.report['workloads']=[]
            p.collect_workloads([sys.executable,str(script)],self.report,out,timeout=0.3)
            self.assertEqual(len(self.report['workloads']),5)
            self.assertEqual(len(self.report['runFailures']),1)
            self.assertEqual(self.report['runFailures'][0]['kind'],'timeout')
            self.assertIn('partial output',(out/'contacts-0.stdout').read_text())
            self.assertIn('partial error',(out/'contacts-0.stderr').read_text())
            self.assertTrue(all(len(w['runs'])==1 for w in self.report['workloads'][1:]))
            self.assertFalse(self.report['complete'])
            self.setUp();self.report['workloads']=[]
            p.collect_workloads([str(out/'missing-executable')],self.report,out)
            self.assertEqual(len(self.report['runFailures']),5)
            self.assertTrue(all(f['kind']=='launch' for f in self.report['runFailures']))
            self.assertFalse(self.report['complete'])
    def test_nearest_rank_and_invalid_samples(self):
        self.assertEqual(p.percentile([1,2,3,4],0.95),4)
        for samples in ([],[float('nan')],[-1],[float('inf')]):
            with self.assertRaises(ValueError):p.percentile(samples,0.95)
    def test_budget_pass_and_breach(self):
        self.assertEqual(p.evaluate(self.report,self.budgets)['status'],'passed')
        self.report['workloads'][0]['runs'][0]['peakRSSBytes']=999999999
        self.assertEqual(p.evaluate(self.report,self.budgets)['status'],'failed')
    def test_missing_sample_and_workload_fail_closed(self):
        self.report['workloads'][0]['runs'][0]['metrics']['coldReadUs']=[]
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();self.report['workloads'].pop()
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
    def test_smoke_and_simulator_do_not_qualify_hardware(self):
        self.report['protocol']['mode']='smoke'
        self.report['workloads'][0]['runs'][0]['peakRSSBytes']=999999999
        self.assertEqual(p.evaluate(self.report,self.budgets)['status'],'unqualified')
        self.report['workloads'][0]['runs'][0]['peakRSSBytes']=1000
        self.report['protocol']['mode']='measure';self.report['environment']['platform']='ios-simulator'
        for w in self.report['workloads']:
            for r in w['runs']:r['platform']='ios-simulator'
        self.assertEqual(p.evaluate(self.report,self.budgets)['status'],'unqualified')
    def test_regression_and_environment_mismatch(self):
        baseline=copy.deepcopy(self.report)
        for run in self.report['workloads'][0]['runs']:run['metrics']['hotReadUs']=[1.2]*100
        self.assertEqual(p.evaluate(self.report,self.budgets,baseline)['status'],'failed')
        baseline['environment']['hardware']='different'
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets,baseline)
    def test_under_sampled_or_wrong_scale_cannot_pass(self):
        self.report['protocol']['nodes']=100
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
        self.setUp();self.report['protocol']['runs']=2
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets)
    def resource_run_fixture(self, name, nodes=10000):
        spec=importlib.util.spec_from_file_location('resource_fixture',Path(__file__).with_name('test-resource-measurements.py'))
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        return next(w['runs'][0] for w in module.report_fixture(2)['workloads'] if w['name']==name)
    def collect_resource_fixture(self, root, fail=False, changed=False):
        worker=root/'worker.py';fixtures=root/'fixtures.json';output=root/'results'
        fixtures.write_text(json.dumps({name:self.resource_run_fixture(name) for name in p.WORKLOADS}))
        worker.write_text('#!'+sys.executable+'\nimport json,sys\nfrom pathlib import Path\n'
          +("if sys.argv[1]=='contacts':\n print('partial output');print('partial error',file=sys.stderr);sys.exit(7)\n" if fail else '')
          +"print(json.dumps(json.loads(Path(__file__).with_name('fixtures.json').read_text())[sys.argv[1]]))\n")
        worker.chmod(0o755)
        def environment(args, **kwargs):
            if args[:2]==['git','rev-parse']:return 'a'*40
            if args[:2]==['git','status']:return ''
            return 'fixture'
        with patch.object(p,'command',side_effect=environment), patch.object(p,'build',return_value=worker) as build, \
             patch.object(p,'source_digest',side_effect=['b'*64,('c' if changed else 'b')*64]), \
             patch.object(p,'evaluate',side_effect=AssertionError('resource collection called budget evaluation')), \
             patch.object(sys,'argv',['performance.py','run','--mode','resources','--output',str(output)]), contextlib.redirect_stdout(io.StringIO()):
            result=p.main()
            build.assert_called_once_with(output.resolve(),False,resources=True)
        return result,json.loads((output/'results.json').read_text()),output
    def test_resource_cli_collects_fixed_protocol_without_budget_evaluation(self):
        with tempfile.TemporaryDirectory() as directory:
            result,report,out=self.collect_resource_fixture(Path(directory))
            self.assertEqual(result,0)
            self.assertEqual(report['protocol'],p.measurement_protocol(self.budgets,'desktop','resources'))
            self.assertEqual(report['evaluation']['status'],'diagnostic-only')
            self.assertFalse(report['evaluation']['hardwareQualified'])
            self.assertEqual(len(report['evaluation']['tables']),5)
            self.assertTrue(report['complete'])
            self.assertEqual(json.loads((out/'knowledge-0.stdout').read_text())['dataset']['name'],'knowledge')
            self.assertEqual((out/'knowledge-0.stderr').read_text(),'')
    def test_resource_cli_rejects_baseline_before_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'unused'
            args=argparse.Namespace(mode='resources',output=output,baseline=Path(directory)/'baseline.json',profile='desktop',platform='macos')
            with patch.object(p,'build',side_effect=AssertionError('baseline rejection happened after build')), \
                 patch.object(p,'command',return_value='fixture'), self.assertRaisesRegex(ValueError,'baseline'):p.run(args)
            self.assertFalse(output.exists())
    def test_instrumented_runs_cannot_qualify_even_if_relabeled(self):
        self.report['workloads'][0]['runs'][0]['resources']={'instrumented':True}
        with self.assertRaisesRegex(ValueError,'instrumented|resource'):p.evaluate(self.report,self.budgets)
        baseline=copy.deepcopy(self.report);self.setUp()
        with self.assertRaises(ValueError):p.evaluate(self.report,self.budgets,baseline)
    def test_resource_failures_retain_raw_output_and_report(self):
        with tempfile.TemporaryDirectory() as directory:
            result,report,out=self.collect_resource_fixture(Path(directory),fail=True)
            self.assertEqual(result,1);self.assertFalse(report['complete'])
            self.assertEqual(report['evaluation']['status'],'failed')
            self.assertEqual(report['runFailures'][0]['exitCode'],7)
            self.assertIn('partial output',(out/'contacts-0.stdout').read_text())
            self.assertIn('partial error',(out/'contacts-0.stderr').read_text())
            self.assertEqual(len(report['workloads'][-1]['runs']),1)
    def test_collection_source_changes_invalidate_retained_report(self):
        with tempfile.TemporaryDirectory() as directory:
            result,report,out=self.collect_resource_fixture(Path(directory),changed=True)
            self.assertEqual(result,1);self.assertFalse(report['complete'])
            self.assertEqual(report['runFailures'][0]['kind'],'source-changed')
            self.assertTrue((out/'recommendations-0.stdout').exists())
    def test_resource_build_flag_does_not_change_timing_build(self):
        with tempfile.TemporaryDirectory() as directory:
            out=Path(directory)
            with patch.object(p,'command',return_value='/sdk'),patch.object(p.subprocess,'run') as compile:
                p.build(out,False)
                self.assertNotIn('-DNOVA_RESOURCE_DIAGNOSTICS',compile.call_args.args[0])
                p.build(out,False,resources=True)
                self.assertIn('-DNOVA_RESOURCE_DIAGNOSTICS',compile.call_args.args[0])
                self.assertEqual(compile.call_args.args[0][:2],['xcrun','clang++'])
    def test_source_digest_tracks_benchmark_code_without_private_notes(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'tools').mkdir();(root/'benchmarks/performance').mkdir(parents=True)
            script=root/'tools/performance.py';script.write_text('collector')
            (root/'tools/resource_measurements.py').write_text('validator')
            (root/'tools/performance_lock.py').write_text('lock')
            (root/'benchmarks/budgets.json').write_text('{}')
            native=root/'benchmarks/performance/native.cpp';native.write_text('int main() { return 0; }')
            header=root/'benchmarks/performance/ResourceDisk.hpp';header.write_text('#pragma once')
            plan=root/'benchmarks/performance/PLAN.md'
            with patch.object(p,'ROOT',root),patch.object(p,'__file__',str(script)):
                public_digest=p.source_digest()
                plan.write_text('Private implementation notes')
                self.assertEqual(public_digest,p.source_digest())
                plan.write_text('Revised private notes')
                self.assertEqual(public_digest,p.source_digest())
                plan.unlink()
                self.assertEqual(public_digest,p.source_digest())
                native.write_text('int main() { return 1; }')
                source_digest=p.source_digest()
                self.assertNotEqual(public_digest,source_digest)
                header.write_text('#pragma once\n#define SAMPLE_COUNT 100')
                self.assertNotEqual(source_digest,p.source_digest())
    def test_resource_validator_is_in_source_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'tools').mkdir();(root/'benchmarks').mkdir()
            script=root/'tools/performance.py';script.write_text('collector')
            (root/'benchmarks/budgets.json').write_text('{}')
            validator=root/'tools/resource_measurements.py';validator.write_text('before')
            (root/'tools/performance_lock.py').write_text('lock')
            with patch.object(p,'ROOT',root),patch.object(p,'__file__',str(script)):
                before=p.source_digest();validator.write_text('after')
                self.assertNotEqual(before,p.source_digest())
                before=p.source_digest();(root/'tools/performance_lock.py').write_text('changed lock')
                self.assertNotEqual(before,p.source_digest())
if __name__=='__main__':unittest.main()
