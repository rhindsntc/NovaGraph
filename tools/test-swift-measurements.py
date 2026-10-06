#!/usr/bin/env python3
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import subprocess

path=Path(__file__).with_name('swift-measurements.py')
spec=importlib.util.spec_from_file_location('swift_measurements',path)
m=importlib.util.module_from_spec(spec) if path.exists() else None
if m:spec.loader.exec_module(m)

class SwiftMeasurementContracts(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(m,'Swift measurement collector is missing')
        self.run={'schemaVersion':2,'validated':True,'orderOffset':0,'warmup':20,'samples':100,'scenarios':[{'payloadBytes':size,'nodes':32,'visitedNodes':32,'cTransportUs':[1]*100,'cJSONUs':[2]*100,'swiftTypedUs':[3]*100} for size in (64,512,1024)]}
        self.report={'schemaVersion':1,'kind':'swift-read-diagnostic','revision':'a'*40,'sourceDigest':'b'*64,'dirty':False,'complete':True,'runFailures':[],'protocol':{'runs':20,'samples':100,'warmup':20,'nodes':32,'payloadBytes':[64,512,1024]},'environment':{'platform':'macos','hardware':'test','os':'test','architecture':'arm64','toolchain':'test','configuration':'release'},'runs':[dict(copy.deepcopy(self.run),orderOffset=i%3) for i in range(20)]}
    def test_summary_preserves_lanes_and_does_not_qualify(self):
        result=m.summarize(self.report)
        self.assertEqual(result['status'],'diagnostic-only')
        self.assertEqual(len(result['tables']),3)
        self.assertEqual(result['tables'][0]['swiftTypedUs'],{'p50':3,'p95':3,'p99':3,'samples':2000})
        self.assertEqual(result['tables'][1]['cJSONUs']['p95'],2)
    def test_corrupt_samples_and_protocol_fail_closed(self):
        for lane in ('cTransportUs','cJSONUs','swiftTypedUs'):
            for values in ([1]*99,[True]*100,[float('nan')]*100,[-1]*100):
                run=copy.deepcopy(self.run);run['scenarios'][0][lane]=values
                with self.assertRaises(ValueError):m.validate_run(run,0)
        for key,value in [('validated',False),('orderOffset',1),('warmup',0),('samples',1)]:
            run=copy.deepcopy(self.run);run[key]=value
            with self.assertRaises(ValueError):m.validate_run(run,0)
        for key,value in [('nodes',1),('visitedNodes',1),('payloadBytes',65)]:
            run=copy.deepcopy(self.run);run['scenarios'][0][key]=value
            with self.assertRaises(ValueError):m.validate_run(run,0)
        run=copy.deepcopy(self.run);run['scenarios'].pop()
        with self.assertRaises(ValueError):m.validate_run(run,0)
    def test_incomplete_or_conflicting_evidence_has_no_summary(self):
        for key,value in [('complete',False),('runFailures',[{'error':'failed'}]),('revision','bad'),('sourceDigest','bad'),('dirty',None),('runs',[])]:
            report=copy.deepcopy(self.report);report[key]=value
            with self.assertRaises(ValueError):m.summarize(report)
        for key,value in [('platform','ios-device'),('configuration','debug')]:
            report=copy.deepcopy(self.report);report['environment'][key]=value
            with self.assertRaises(ValueError):m.summarize(report)
        report=copy.deepcopy(self.report);report['protocol']['runs']=2
        with self.assertRaises(ValueError):m.summarize(report)
    def expanded(self):
        report=copy.deepcopy(self.report)
        report.update(schemaVersion=2,kind='swift-diagnostic')
        report['protocol']['operations']={'version':1,'mode':'measure','warmup':20,'samples':100,'batchMutations':250,'traversalNodes':32,'traversalDepth':2,'traversalLimit':100,'graph':'directed-ring','cold':'unique-after-forced-trim','durability':'default-durable-receipt','mutation':'replace-string-and-integer-properties','asyncConcurrency':1,'phaseOrder':['cold','single','batch','traversal','asyncRead','asyncTraversal','asyncMutation']}
        report['protocolDigest']=hashlib.sha256(json.dumps(report['protocol'],sort_keys=True,separators=(',', ':')).encode()).hexdigest()
        for run in report['runs']:
            run['schemaVersion']=3
            run['operationsProtocol']=copy.deepcopy(report['protocol']['operations'])
            run['operationScenarios']=[{'payloadBytes':size,'coldNodes':120,'coldEvicted':120,'coldVisited':120,'reopenValidated':True,'batchMutations':250,'traversalNodes':32,'traversalEdges':32,'traversalResults':2,**{lane:[4]*100 for lane in ('swiftColdUs','swiftSingleMutationUs','swiftBatch250Us','swiftTraversalUs','swiftAsyncReadUs','swiftAsyncTraversalUs','swiftAsyncMutationUs')}} for size in (64,512,1024)]
        return report
    def test_expanded_diagnostics_summarize_processes_without_native_caps(self):
        result=m.summarize(self.expanded())
        self.assertEqual(result['status'],'diagnostic-only')
        self.assertEqual(result['operationTables'][0]['swiftColdUs']['samples'],2000)
        self.assertEqual(result['operationTables'][0]['swiftBatch250Us']['processP95']['samples'],20)
        self.assertEqual(result['operationTables'][0]['swiftBatch250Us']['processP95']['p95'],4)
    def test_operation_summary_distinguishes_pooled_samples_from_process_tails(self):
        report=self.expanded()
        for index,run in enumerate(report['runs']):
            run['operationScenarios'][0]['swiftColdUs']=[1]*85+[1000 if index<3 else 1]*15
        lane=m.summarize(report)['operationTables'][0]['swiftColdUs']
        self.assertEqual(lane['p95'],1)
        self.assertEqual(lane['processMedian']['p95'],1)
        self.assertEqual(lane['processP95']['p95'],1000)
    def test_expanded_protocol_digest_rejects_mismatch(self):
        report=self.expanded();report['protocolDigest']='0'*64
        with self.assertRaises(ValueError):m.summarize(report)
    def test_expanded_missing_wrong_contract_and_nonfinite_metrics_fail(self):
        for mutate in (lambda r:r['runs'][0]['operationScenarios'].pop(), lambda r:r['runs'][0]['operationScenarios'][0].update(coldVisited=119),lambda r:r['runs'][0]['operationScenarios'][0].update(reopenValidated=False),lambda r:r['runs'][0]['operationScenarios'][0].update(swiftBatch250Us=[float('inf')]*100),lambda r:r['runs'][0]['operationsProtocol'].update(mode='contract'),lambda r:r['protocol']['operations'].update(batchMutations=249),lambda r:r['runs'][0].update(schemaVersion=2)):
            report=self.expanded();mutate(report)
            with self.assertRaises(ValueError):m.summarize(report)
    def test_collector_retains_failure_output_and_stops(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder);worker=out/'worker.py'
            worker.write_text("import sys\nprint('partial output',flush=True)\nprint('failure detail',file=sys.stderr)\nsys.exit(7)\n")
            report=copy.deepcopy(self.report);report['runs']=[];report['complete']=False
            m.collect([sys.executable,str(worker)],report,out)
            self.assertFalse(report['complete']);self.assertEqual(report['runs'],[])
            self.assertEqual(report['runFailures'][0]['exitCode'],7)
            self.assertIn('partial output',(out/'run-0.stdout').read_text())
            self.assertIn('failure detail',(out/'run-0.stderr').read_text())
    def test_metadata_command_failure_retains_report(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'evidence'
            with patch.object(sys,'argv',['swift-measurements.py','--output',str(out)]), patch.object(m.performance,'command',side_effect=subprocess.CalledProcessError(1,['git','rev-parse','HEAD'])):
                self.assertEqual(m.main(),1)
            report=json.loads((out/'results.json').read_text())
            self.assertFalse(report['complete'])
            self.assertEqual(report['runFailures'][0]['kind'],'collector')
            self.assertIn('git',report['runFailures'][0]['error'])
    def test_collector_retains_timeout_and_invalid_output(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder);worker=out/'worker.py'
            for source,kind in [("import time\nprint('partial',flush=True)\ntime.sleep(5)",'timeout'),("print('{}')",'invalid-output')]:
                worker.write_text(source)
                report=copy.deepcopy(self.report);report['runs']=[];report['complete']=False
                m.collect([sys.executable,str(worker)],report,out,timeout=0.3)
                self.assertFalse(report['complete']);self.assertEqual(report['runFailures'][0]['kind'],kind)
                self.assertTrue((out/'run-0.stdout').read_text())
    def test_approved_swift_caps_have_inclusive_boundaries(self):
        budgets=json.loads((m.ROOT/'benchmarks/budgets.json').read_text())
        for lane,limit in [('swiftTypedUs',2000),('swiftColdUs',10000),('swiftSingleMutationUs',10000),('swiftBatch250Us',50000),('swiftTraversalUs',5000),('swiftAsyncReadUs',2000),('swiftAsyncTraversalUs',5000),('swiftAsyncMutationUs',10000)]:
            report=self.expanded()
            key='scenarios' if lane=='swiftTypedUs' else 'operationScenarios'
            for run in report['runs']:run[key][2][lane]=[limit]*100
            self.assertEqual(m.evaluate_budget(report,budgets,'a'*40,'b'*64)['status'],'passed')
            for run in report['runs']:run[key][2][lane]=[limit+1]*100
            self.assertEqual(m.evaluate_budget(report,budgets,'a'*40,'b'*64)['status'],'failed')
    def test_swift_caps_do_not_hide_independent_process_tails(self):
        report=self.expanded()
        for run in report['runs']:run['operationScenarios'][0]['swiftColdUs']=[1]*100
        for run in report['runs'][:3]:run['operationScenarios'][0]['swiftColdUs']=[1]*85+[10001]*15
        result=m.evaluate_budget(report,json.loads((m.ROOT/'benchmarks/budgets.json').read_text()),'a'*40,'b'*64)
        self.assertEqual(result['status'],'failed')
        row=next(x for x in result['tables'] if x['payloadBytes']==64 and x['metric']=='swiftColdUs')
        self.assertEqual(row['pooledP95'],1)
        self.assertEqual(row['processP95'],10001)
        self.assertFalse(result['regressionQualified'])
    def test_swift_budget_rejects_dirty_foreign_historical_and_unapproved_evidence(self):
        budgets=json.loads((m.ROOT/'benchmarks/budgets.json').read_text())
        for key,value in [('dirty',True),('revision','c'*40),('sourceDigest','d'*64),('complete',False),('protocolDigest','0'*64)]:
            report=self.expanded();report[key]=value
            with self.assertRaises(ValueError):m.evaluate_budget(report,budgets,'a'*40,'b'*64)
        with self.assertRaises(ValueError):m.evaluate_budget(self.report,budgets,'a'*40,'b'*64)
        bad=copy.deepcopy(budgets);bad['swift']={'review':{'status':'proposed'}}
        with self.assertRaises(ValueError):m.evaluate_budget(self.expanded(),bad,'a'*40,'b'*64)
        self.assertEqual(m.summarize(self.expanded())['status'],'diagnostic-only')
if __name__=='__main__':unittest.main()
