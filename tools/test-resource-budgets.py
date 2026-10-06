#!/usr/bin/env python3
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch
import resource_budgets as m
spec=importlib.util.spec_from_file_location('fixture',Path(__file__).with_name('test-resource-measurements.py'))
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)

class ResourceBudgetContracts(unittest.TestCase):
    def setUp(self):
        self.report=f.report_fixture(2)
        self.budgets=json.loads((m.p.ROOT/'benchmarks/budgets.json').read_text())
    def evaluate(self):
        return m.evaluate_disk(self.report,self.budgets,'a'*40,'b'*64)
    def test_all_boundaries_have_inclusive_caps_and_earlier_breaches_fail(self):
        for key,budget in [('logicalBytes','diskLogicalBytes'),('allocatedBytes','diskAllocatedBytes'),('files','diskFiles')]:
            self.setUp()
            snapshot=self.report['workloads'][0]['runs'][0]['resources']['diskSnapshots'][0]
            snapshot[key]=snapshot['categories']['payload'][key]=self.budgets['classes']['desktop'][budget]
            self.assertEqual(self.evaluate()['status'],'passed')
            snapshot[key]+=1;snapshot['categories']['payload'][key]+=1
            result=self.evaluate();self.assertEqual(result['status'],'failed')
            self.assertIn('contacts '+budget+' exceeds budget',result['failures'])
            self.assertFalse(result['hardwareQualified'])
    def test_unavailable_allocation_does_not_become_zero(self):
        snapshot=self.report['workloads'][0]['runs'][0]['resources']['diskSnapshots'][0]
        snapshot.update(allocatedBytes=None,allocationSupport='unavailable')
        for row in snapshot['categories'].values():row['allocatedBytes']=None
        result=self.evaluate();self.assertEqual(result['status'],'unqualified')
        self.assertEqual(result['unavailable'],['contacts diskAllocatedBytes'])
    def test_known_allocation_breach_fails_even_with_other_unavailable_snapshot(self):
        snapshots=self.report['workloads'][0]['runs'][0]['resources']['diskSnapshots']
        snapshots[0]['allocatedBytes']=snapshots[0]['categories']['payload']['allocatedBytes']=self.budgets['classes']['desktop']['diskAllocatedBytes']+1
        snapshots[1].update(allocatedBytes=None,allocationSupport='unavailable')
        for row in snapshots[1]['categories'].values():row['allocatedBytes']=None
        result=self.evaluate()
        self.assertEqual(result['status'],'failed')
        self.assertIn('contacts diskAllocatedBytes exceeds budget',result['failures'])
        self.assertIn('contacts diskAllocatedBytes',result['unavailable'])

    def test_dirty_foreign_incomplete_and_historical_reports_fail_closed(self):
        for key,value in [('dirty',True),('revision','c'*40),('sourceDigest','d'*64),('complete',False)]:
            self.setUp();self.report[key]=value
            with self.assertRaises(ValueError):self.evaluate()
        self.report=f.report_fixture(1)
        with self.assertRaises(ValueError):self.evaluate()
    def test_missing_snapshot_or_mismatched_totals_fail(self):
        self.report['workloads'][0]['runs'][0]['resources']['diskSnapshots'].pop()
        with self.assertRaises(ValueError):self.evaluate()
        self.setUp();self.report['workloads'][0]['runs'][0]['resources']['diskSnapshots'][0]['files']+=1
        with self.assertRaises(ValueError):self.evaluate()
    def test_distribution_revalidates_before_applying_inclusive_caps(self):
        manifest={'revision':'a'*40,'sourceDigest':'b'*64,'dirty':False}
        sizes={key:self.budgets['distribution'][key] for key in ('archiveBytes','bundleLogicalBytes')}
        events=[]
        def verify(bundle):events.append('verify');return manifest
        def measure(bundle,checked,archive):
            self.assertIs(checked,manifest);events.append('measure');return sizes
        with patch.object(m.apple,'verify_bundle',side_effect=verify),patch.object(m.apple,'resource_size_report',side_effect=measure):
            self.assertEqual(m.evaluate_distribution(Path('bundle'),Path('zip'),self.budgets,'a'*40,'b'*64)['status'],'passed')
            self.assertEqual(events,['verify','measure'])
            for key in sizes:
                sizes[key]+=1
                self.assertEqual(m.evaluate_distribution(Path('bundle'),Path('zip'),self.budgets,'a'*40,'b'*64)['status'],'failed')
                sizes[key]-=1
    def test_distribution_verification_or_provenance_failure_prevents_gate(self):
        with patch.object(m.apple,'verify_bundle',side_effect=ValueError('corrupt')) as verify,patch.object(m.apple,'resource_size_report') as measure:
            with self.assertRaises(ValueError):m.evaluate_distribution('bundle','zip',self.budgets,'a'*40,'b'*64)
            measure.assert_not_called()
        with patch.object(m.apple,'verify_bundle',return_value={'dirty':False,'revision':'c'*40,'sourceDigest':'b'*64}),patch.object(m.apple,'resource_size_report') as measure:
            with self.assertRaises(ValueError):m.evaluate_distribution('bundle','zip',self.budgets,'a'*40,'b'*64)
            measure.assert_not_called()

    def test_linked_app_caps_are_inclusive_after_reverification(self):
        verified={'revision':'a'*40,'sourceDigest':'b'*64,'dirty':False,'deltas':[{'platform':p,'bundleLogicalBytes':2097152} for p in ('macos','ios')]}
        with patch.object(m.linked,'verify',return_value=verified) as verify:
            result=m.evaluate_linked(Path('report.json'),self.budgets,'a'*40,'b'*64)
            self.assertEqual(result['status'],'passed');verify.assert_called_once()
            verified['deltas'][1]['bundleLogicalBytes']+=1
            self.assertEqual(m.evaluate_linked(Path('report.json'),self.budgets,'a'*40,'b'*64)['status'],'failed')
            self.assertFalse(result['hardwareQualified'])
    def test_linked_app_mismatch_or_unapproved_caps_cannot_pass(self):
        with patch.object(m.linked,'verify',side_effect=ValueError('tampered bundle')):
            with self.assertRaises(ValueError):m.evaluate_linked(Path('report.json'),self.budgets,'a'*40,'b'*64)
        bad=copy.deepcopy(self.budgets);bad['linkedApp']={'review':{'status':'proposed'}}
        with self.assertRaises(ValueError):m.evaluate_linked(Path('report.json'),bad,'a'*40,'b'*64)
    def test_approved_extension_limits_reject_invalid_scope_and_values(self):
        for section,key,value in [('swift','version',True),('swift','platform','ios'),('swift','payloadBytes',[64,512]),('linkedApp','platforms',['macos']),('linkedApp','bundleLogicalBytes',True),('linkedApp','bundleLogicalBytes',float('inf'))]:
            bad=copy.deepcopy(self.budgets);bad[section][key]=value
            with self.assertRaises(ValueError):m.p.validate_budgets(bad)
        for value in (0,-1,True,float('nan')):
            bad=copy.deepcopy(self.budgets);bad['swift']['metrics']['swiftColdUs']=value
            with self.assertRaises(ValueError):m.p.validate_budgets(bad)

    def linked_fixture(self,out):
        out=out.resolve()
        report={'schemaVersion':1,'kind':'linked-app-size','revision':'a'*40,'sourceDigest':'b'*64,'dirty':False,'complete':True,'failures':[],
                'protocol':m.linked.PROTOCOL,'protocolDigest':m.linked.PROTOCOL_DIGEST,
                'environment':{'xcode':'test','sdks':{'macos':'27.0','ios':'27.0'}},'results':[]}
        for platform in ('macos','ios'):
            for variant in ('baseline','nova'):
                directory=out/(platform+'-'+variant);directory.mkdir()
                (directory/'App.swift').write_bytes(m.linked.FIXTURE.read_bytes())
                (directory/'project.json').write_text(json.dumps(m.linked.project_spec(platform,variant)))
                project=directory/'Footprint.xcodeproj';project.mkdir()
                (project/'project.pbxproj').write_text('App.swift in Sources'+(' GraphDBKit in Frameworks' if variant=='nova' else ''))
                (directory/'build.log').write_text('Strip Footprint\n** BUILD SUCCEEDED **\n')
                product=directory/m.linked.product_path(platform);product.mkdir(parents=True)
                binary=product/('Contents/MacOS/Footprint' if platform=='macos' else 'Footprint')
                binary.parent.mkdir(parents=True,exist_ok=True);binary.write_bytes(b'x'*(10 if variant=='baseline' else 14))
                report['results'].append(m.linked.inspect(directory,platform,variant))
        path=out/'results.json';path.write_text(json.dumps(report));return path

    def linked_command(self,args,**kwargs):
        if 'lipo' in args:return 'arm64'
        if 'otool' in args:
            platform='IOS' if 'ios-' in str(args[-1]) else 'MACOS'
            return 'cmd LC_BUILD_VERSION\ncmdsize 24\nplatform '+platform+'\nminos '+('16.0' if platform=='IOS' else '13.0')
        if '-showBuildSettings' in args:
            variant='nova' if 'nova' in str(args) else 'baseline'
            platform='ios' if 'iphoneos' in args else 'macos'
            settings=dict(m.linked.SETTINGS,CONFIGURATION='Release',ARCHS='arm64',PLATFORM_NAME='iphoneos' if platform=='ios' else 'macosx',SWIFT_ACTIVE_COMPILATION_CONDITIONS='WITH_NOVA' if variant=='nova' else '')
            return json.dumps([{'target':'Footprint','buildSettings':settings}])
        return 'verified-nova' if 'nova' in str(args) else 'verified-baseline'

    def test_linked_verifier_rehashes_real_files_and_recalculates_deltas(self):
        with tempfile.TemporaryDirectory() as folder,patch.object(m.p,'command',side_effect=self.linked_command):
            path=self.linked_fixture(Path(folder))
            report=m.linked.verify(path,'a'*40,'b'*64)
            self.assertEqual([row['bundleLogicalBytes'] for row in report['deltas']],[4,4])
            # A different file with the same byte count is still altered evidence.
            binary=path.parent/'ios-nova'/m.linked.product_path('ios')/'Footprint'
            binary.write_bytes(b'y'*14)
            with self.assertRaises(ValueError):m.linked.verify(path,'a'*40,'b'*64)

    def test_linked_verifier_rejects_foreign_incomplete_and_mismatched_metadata(self):
        with tempfile.TemporaryDirectory() as folder,patch.object(m.p,'command',side_effect=self.linked_command):
            path=self.linked_fixture(Path(folder));original=json.loads(path.read_text())
            for key,value in [('dirty',True),('revision','c'*40),('sourceDigest','d'*64),('complete',False),('protocolDigest','0'*64),('results',original['results'][:3])]:
                bad=copy.deepcopy(original);bad[key]=value;path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):m.linked.verify(path,'a'*40,'b'*64)
            path.write_text(json.dumps(original))
            fixture=path.parent/'macos-baseline/App.swift';fixture.write_text('different app')
            with self.assertRaises(ValueError):m.linked.verify(path,'a'*40,'b'*64)

    def test_linked_verifier_rejects_symlinks_and_wrong_binary_platform(self):
        with tempfile.TemporaryDirectory() as folder,patch.object(m.p,'command',side_effect=self.linked_command):
            path=self.linked_fixture(Path(folder))
            extra=path.parent/'ios-nova'/m.linked.product_path('ios')/'escape';extra.symlink_to(path)
            with self.assertRaises(ValueError):m.linked.verify(path,'a'*40,'b'*64)
            extra.unlink()
            def wrong(args,**kwargs):
                if 'otool' in args:return 'cmd LC_BUILD_VERSION\ncmdsize 24\nplatform IOSSIMULATOR\nminos 16.0'
                return self.linked_command(args,**kwargs)
            with patch.object(m.p,'command',side_effect=wrong):
                with self.assertRaises(ValueError):m.linked.verify(path,'a'*40,'b'*64)

    def test_linked_verifier_rejects_missing_consumed_project_or_nova_flag(self):
        with tempfile.TemporaryDirectory() as folder,patch.object(m.p,'command',side_effect=self.linked_command):
            path=self.linked_fixture(Path(folder));project=path.parent/'ios-nova/Footprint.xcodeproj/project.pbxproj'
            original=project.read_text();project.unlink()
            with self.assertRaises((ValueError,OSError)):m.linked.verify(path,'a'*40,'b'*64)
            project.write_text(original)
            def missing_flag(args,**kwargs):
                value=self.linked_command(args,**kwargs)
                if '-showBuildSettings' in args:
                    settings=json.loads(value);settings[0]['buildSettings']['SWIFT_ACTIVE_COMPILATION_CONDITIONS']='';return json.dumps(settings)
                return value
            with patch.object(m.p,'command',side_effect=missing_flag):
                with self.assertRaises(ValueError):m.linked.verify(path,'a'*40,'b'*64)

if __name__=='__main__':unittest.main()
