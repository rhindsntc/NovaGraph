#!/usr/bin/env python3
"""Diagnostic contracts. Fixtures and disposable child processes, never benchmarks."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import performance_gate as g
import performance as p


class DiagnosticContracts(unittest.TestCase):
    def module(self, name):
        self.assertIsNotNone(importlib.util.find_spec(name), 'missing supported diagnostic module: ' + name)
        return __import__(name)

    def process(self, pid=42, started='Wed Sep 30 12:00:00 2026', cpu=1., role='other'):
        return dict(pid=pid, ppid=1, started=started, command='/tmp/nova-performance', cpuSeconds=cpu, role=role)

    def sample(self, t, cpu, phase='idle', **overrides):
        return dict(monotonic=t, timestamp='2026-09-30T12:00:00+00:00', phase=phase,
                    loadAverage=[2., 2., 2.], samplingSeconds=.01,
                    processes=[self.process(cpu=cpu, **overrides)])

    def test_process_parser_preserves_names_and_rejects_bad_cpu(self):
        a=self.module('performance_activity')
        rows=a.parse_processes('42 1 1:02.50 Wed Sep 30 12:00:00 2026 /Some App/nova-performance')
        self.assertEqual(rows[0]['cpuSeconds'],62.5)
        self.assertEqual(rows[0]['command'],'/Some App/nova-performance')
        for value in ('nan:00','-1:00','1:70','oops'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                a.parse_processes('42 1 '+value+' Wed Sep 30 12:00:00 2026 /bin/x')

    def test_same_name_and_reused_pid_are_not_owned(self):
        a=self.module('performance_activity')
        owned={a.identity(self.process()): 'nova'}
        self.assertEqual(a.classify(self.process(),owned),'nova')
        self.assertEqual(a.classify(self.process(pid=43),owned),'other')
        self.assertEqual(a.classify(self.process(started='Wed Sep 30 12:01:00 2026'),owned),'other')

    def test_cpu_delta_is_not_lifetime_cpu_or_machine_percent(self):
        a=self.module('performance_activity')
        summary=a.summarize([self.sample(0,100,role='nova'),self.sample(2,101,role='nova')])
        self.assertEqual(summary['phases']['idle']['cpuSecondsByRole'],{'nova':1.,'collector':0.,'other':0.})
        self.assertEqual(summary['phases']['idle']['observedSeconds'],2.)

    def test_pid_reuse_and_phase_boundaries_do_not_bridge_deltas(self):
        a=self.module('performance_activity')
        s=[self.sample(0,1),self.sample(2,50,started='Wed Sep 30 13:00:00 2026'),self.sample(4,55,phase='active')]
        summary=a.summarize(s)
        self.assertEqual(sum(x['cpuSecondsByRole']['other'] for x in summary['phases'].values()),0)
        self.assertGreater(summary['unmatchedProcessObservations'],0)

    def test_failed_samples_and_large_gaps_are_visible(self):
        a=self.module('performance_activity')
        s=[self.sample(0,1),dict(monotonic=2,phase='idle',error='ps denied'),self.sample(8,5)]
        summary=a.summarize(s)
        self.assertFalse(summary['coverageComplete'])
        self.assertEqual(summary['sampleErrors'],['ps denied'])
        self.assertEqual(summary['maxGapSeconds'],6)
        self.assertTrue(summary['gaps'])

    def test_monitor_retains_sampler_errors_and_stops(self):
        a=self.module('performance_activity')
        with tempfile.TemporaryDirectory() as td:
            monitor=a.ActivityMonitor(Path(td)/'activity.jsonl')
            with patch.object(a,'capture_sample',side_effect=OSError('fixture denied')):
                monitor.start(); monitor.stop()
            rows=[json.loads(x) for x in monitor.path.read_text().splitlines()]
            self.assertTrue(rows)
            self.assertIn('fixture denied',rows[0]['error'])
            self.assertFalse(monitor.thread.is_alive())

    def test_registration_checks_executable_not_just_pid(self):
        a=self.module('performance_activity')
        with tempfile.TemporaryDirectory() as td:
            monitor=a.ActivityMonitor(Path(td)/'activity.jsonl')
            with patch.object(a,'read_process',return_value=self.process()):
                with self.assertRaisesRegex(ValueError,'executable'):
                    monitor.register(42,'nova','/different/nova-performance')
                monitor.register(42,'nova','/tmp/nova-performance')
                self.assertEqual(a.classify(self.process(),monitor.owners),'nova')

    def test_ownership_discovered_mid_interval_is_reported_not_misattributed(self):
        a=self.module('performance_activity')
        summary=a.summarize([self.sample(0,100,role='other'),self.sample(2,101,role='nova'),self.sample(4,102,role='nova')])
        self.assertTrue(summary['coverageComplete'])
        self.assertEqual(summary['ownershipTransitions'],1)
        self.assertEqual(summary['phases']['idle']['cpuSecondsByRole']['nova'],1.)
        self.assertEqual(summary['phases']['idle']['cpuSecondsByRole']['other'],0.)

    def test_native_summary_rejects_wrong_dataset_and_sample_count(self):
        d=self.module('performance_diagnostic')
        # A valid outcome record cannot make the wrong workload eligible.
        with patch.object(p,'validate_mutation_protocol'):
            with self.assertRaisesRegex(ValueError,'dataset'):
                d.native_summary({'platform':'macos','dataset':{'name':'wrong','nodes':10000,'seed':42}},'navigation')
            with self.assertRaisesRegex(ValueError,'sample count'):
                d.native_summary({'platform':'macos','dataset':{'name':'navigation','nodes':10000,'seed':42},'metrics':{'hotReadUs':[1]}},'navigation')

    def test_summary_reports_spreads_without_qualification(self):
        d=self.module('performance_diagnostic')
        def run(value):return {'workload':'navigation','metrics':{'coldReadUs':{'p50':value,'p95':2*value}}}
        spread=d.timing_spread([run(100),run(120),run(110)])
        self.assertAlmostEqual(spread['navigation']['coldReadUs']['p95']['maxVsMinPercent'],20.)

    def test_diagnostic_kind_is_rejected_even_with_forged_pass_flags(self):
        r={'kind':'nova-activity-diagnostic','complete':True,'hardwareQualified':True}
        with self.assertRaisesRegex(ValueError,'diagnostic'):
            g.validate_report(r,g.budgets())
        with self.assertRaisesRegex(ValueError,'diagnostic'):
            p.evaluate(r,g.budgets())

    def test_cli_exposes_diagnose_but_requires_idle_attestation(self):
        help_result=subprocess.run([sys.executable,'tools/performance_gate.py','diagnose','--help'],capture_output=True,text=True)
        self.assertEqual(help_result.returncode,0,help_result.stderr)
        result=subprocess.run([sys.executable,'tools/performance_gate.py','diagnose','--prepared','unused','--output','unused'],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('--attest-quiet',result.stderr)

    def test_invalid_prepared_retains_unqualified_report_without_sampling(self):
        d=self.module('performance_diagnostic')
        with tempfile.TemporaryDirectory() as td:
            out=Path(td)/'result'
            args=argparse.Namespace(prepared=Path(td)/'absent',output=out,attest_quiet=True,attest_ac_power=True,attest_thermal=True)
            r=d.diagnose(args)
            self.assertFalse(r['diagnosticComplete'])
            self.assertFalse(r['complete'])
            self.assertFalse(r['hardwareQualified'])
            self.assertTrue(r['errors'])
            self.assertEqual(json.loads((out/'diagnostic.json').read_text())['kind'],'nova-activity-diagnostic')

    def test_interrupted_idle_is_retained_without_child_launch(self):
        d=self.module('performance_diagnostic')
        with tempfile.TemporaryDirectory() as td:
            out=Path(td)/'result'; prep=Path(td)/'prepare.json'; prep.write_text('{}')
            args=argparse.Namespace(prepared=Path(td),output=out,attest_quiet=True,attest_ac_power=True,attest_thermal=True)
            with patch.object(g,'validate_prepared'),patch.object(g,'source_identity',return_value={'dirty':False}),patch.object(d,'observe',return_value={}),patch.object(d,'idle',side_effect=KeyboardInterrupt):
                r=d.diagnose(args)
            self.assertFalse(r['diagnosticComplete'])
            self.assertEqual(r['runs'],[])
            self.assertTrue(any('KeyboardInterrupt' in x for x in r['errors']))
            self.assertTrue((out/'diagnostic.json').exists())

    def test_busy_after_idle_prevents_native_launch(self):
        d=self.module('performance_diagnostic')
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); (root/'prepare.json').write_text('{}')
            args=argparse.Namespace(prepared=root,output=root/'result',attest_quiet=True,attest_ac_power=True,attest_thermal=True)
            with patch.object(g,'validate_prepared'),patch.object(g,'source_identity',return_value={'dirty':False}),patch.object(d,'idle'),patch.object(d,'observe',side_effect=[{}, {'gateFailures':['machine busy']}]),patch.object(d,'run_child',side_effect=AssertionError('must not launch')):
                report=d.diagnose(args)
            self.assertFalse(report['diagnosticComplete'])
            self.assertEqual(report['runs'],[])
            self.assertTrue(any('readiness after idle: machine busy' in e for e in report['errors']))

    def test_six_process_lifecycle_preserves_raw_outputs_and_stays_unqualified(self):
        d=self.module('performance_diagnostic');a=self.module('performance_activity')
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); binary=root/'fixture-native'; binary.write_text('fixture')
            (root/'prepare.json').write_text(json.dumps({'binary':{'path':str(binary)}}))
            args=argparse.Namespace(prepared=root,output=root/'result',attest_quiet=True,attest_ac_power=True,attest_thermal=True)
            actual_read=a.read_process
            def read(pid):
                if pid==42:return dict(pid=42,ppid=os.getpid(),started='fixture-start',command=str(binary),cpuSeconds=0.)
                return actual_read(pid)
            def execute(command,stdout,stderr,on_start):
                self.assertEqual(command[2:],['10000','42','100','20'])
                on_start(argparse.Namespace(pid=42))
                native={'platform':'macos','protocolVersion':2,'mutationBatchSize':250,
                    'dataset':{'name':command[1],'nodes':10000,'seed':42},
                    'mutationProtocol':{'records':250,'nodePropertyCount':1,'scalarType':'int64','operation':'upsert-existing-node','durability':'wal-sync','database':'dedicated','samples':100,'warmup':20,'validation':'each-commit-and-reopen'},
                    'mutationValidation':{'singleCommits':120,'batchCommits':120,'mutations':30120,'reopenedRecords':250,'reopenVerified':True},
                    'metrics':{name:[100.]*(100 if name in ('hotReadUs','coldReadUs','novaTraversalUs','singleMutationUs','batchMutationUs') else 1) for name in p.METRICS},'peakRSSBytes':1000}
                Path(stdout).write_text(json.dumps(native));Path(stderr).write_text('')
                return 0
            with patch.object(g,'validate_prepared'),patch.object(g,'source_identity',return_value={'dirty':False}),patch.object(d,'idle'),patch.object(d,'observe',return_value={'gateFailures':[]}),patch.object(a,'read_process',side_effect=read),patch.object(d,'run_child',side_effect=execute):
                report=d.diagnose(args)
            self.assertTrue(report['diagnosticComplete'],report['errors'])
            self.assertFalse(report['complete']);self.assertFalse(report['hardwareQualified'])
            self.assertEqual([r['workload'] for r in report['runs']],['navigation','recommendations','navigation','recommendations','navigation','recommendations'])
            self.assertEqual(set(report['observations']),{'idleStart','beforeActive','immediatePostflight','recoveryEnd'})
            self.assertEqual(len(report['files']),13)
            for record in report['files']:g.verify_record(record)
            self.assertEqual(report['timingSpread']['navigation']['coldReadUs']['p95']['processes'],3)
            with self.assertRaisesRegex(ValueError,'diagnostic'):g.validate_report(report,g.budgets())

    def test_sigterm_retains_incomplete_idle_report(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); (root/'prepare.json').write_text('{}')
            program = """
import argparse, pathlib, sys, time
from unittest.mock import patch
sys.path.insert(0,'tools')
import performance_diagnostic as d
import performance_gate as g
root=pathlib.Path(sys.argv[1])
def wait(seconds):
    (root/'ready').write_text('ready')
    time.sleep(30)
a=argparse.Namespace(prepared=root,output=root/'result',attest_quiet=True,attest_ac_power=True,attest_thermal=True)
with patch.object(g,'validate_prepared'), patch.object(g,'source_identity',return_value={'dirty':False}), patch.object(d,'observe',return_value={}), patch.object(d,'idle',side_effect=wait):
    d.diagnose(a)
"""
            child=subprocess.Popen([sys.executable,'-c',program,td],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
            try:
                deadline=time.monotonic()+5
                while not (root/'ready').exists() and child.poll() is None and time.monotonic()<deadline:time.sleep(.01)
                self.assertTrue((root/'ready').exists())
                child.terminate(); child.communicate(timeout=5)
                self.assertTrue((root/'result/diagnostic.json').exists(),'termination lost partial evidence')
                report=json.loads((root/'result/diagnostic.json').read_text())
                self.assertFalse(report['diagnosticComplete'])
                self.assertTrue(any('SIGTERM' in e for e in report['errors']))
            finally:
                if child.poll() is None:child.kill();child.wait()

    def test_signal_handler_unwinds_child_cleanup_and_restores_handlers(self):
        import signal
        d=self.module('performance_diagnostic')
        self.assertTrue(hasattr(d,'interruption_handlers'),'missing scoped signal handling')
        previous=signal.getsignal(signal.SIGTERM)
        with tempfile.TemporaryDirectory() as td:
            pidfile=Path(td)/'pid'
            # Child sends a real signal to its parent after writing its own identity.
            program='import os,signal,time,pathlib; pathlib.Path('+repr(str(pidfile))+').write_text(str(os.getpid())); os.kill(os.getppid(),signal.SIGTERM); time.sleep(30)'
            with self.assertRaises(KeyboardInterrupt):
                with d.interruption_handlers():
                    d.run_child([sys.executable,'-c',program],Path(td)/'stdout',Path(td)/'stderr',lambda c:None)
            self.assertEqual(signal.getsignal(signal.SIGTERM),previous)
            with self.assertRaises(ProcessLookupError):os.kill(int(pidfile.read_text()),0)

    def test_timeout_terminates_disposable_child(self):
        d=self.module('performance_diagnostic')
        with tempfile.TemporaryDirectory() as td:
            pidfile=Path(td)/'pid'
            cmd=[sys.executable,'-c','import os,time,pathlib; pathlib.Path('+repr(str(pidfile))+').write_text(str(os.getpid())); time.sleep(30)']
            with self.assertRaises(subprocess.TimeoutExpired):
                d.run_child(cmd,Path(td)/'stdout',Path(td)/'stderr',lambda child:None,timeout=.3)
            pid=int(pidfile.read_text())
            with self.assertRaises(ProcessLookupError):os.kill(pid,0)

if __name__=='__main__':unittest.main()
