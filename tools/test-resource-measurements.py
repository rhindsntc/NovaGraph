#!/usr/bin/env python3
import copy
import importlib.util
from pathlib import Path
import unittest
path=Path(__file__).with_name('resource_measurements.py')
spec=importlib.util.spec_from_file_location('resources',path)
m=importlib.util.module_from_spec(spec) if path.exists() else None
if m:spec.loader.exec_module(m)

def snapshot(stage):
    categories={k:{'logicalBytes':0,'allocatedBytes':0,'files':0} for k in ('payload','catalog','wal','other')}
    categories['payload']={'logicalBytes':10,'allocatedBytes':4096,'files':1}
    return {'stage':stage,'logicalBytes':10,'allocatedBytes':4096,'files':1,'categories':categories,'allocationSupport':'st_blocks_512'}
def resource_fixture(version=1):
    resource= {'schemaVersion':1,'instrumented':True,'coverage':'C++ new; explicit request contexts; regular-file snapshots',
      'phases':{p:{'samples':100 if p in ('hotReadUs','coldReadUs','novaTraversalUs') else 3 if p=='maintenanceUs' else 1,'allocation':{'calls':2,'totalBytes':100,'peakBytes':80,'maxRetainedBytes':30},'workspace':{'requests':1,'maxPeakBytes':60,'maxCurrentBytes':10,'maxLimitBytes':16777216}} for p in ('setupUs','hotReadUs','coldReadUs','novaTraversalUs','trimUs','maintenanceUs','checkpointUs','closeUs','openUs')},
      'diskSnapshots':[snapshot(s) for s in ('after-trim','maintenance-0','maintenance-1','maintenance-2','final-checkpoint','after-close','after-reopen')]}
    if version==2:
        resource['schemaVersion']=2
        resource['workspaceProbes']={name:{'api':'execute_dsl','status':'observed',
            'workspace':{'requests':1,'maxPeakBytes':60,'maxCurrentBytes':10,'maxLimitBytes':16777216}}
            for name in ('traversal','checkpoint')}
        for phase in ('singleMutationUs','batchMutationUs'):
            resource['phases'][phase]=copy.deepcopy(resource['phases']['hotReadUs'])
    return resource
def report_fixture(version=1):
    report={'schemaVersion':1,'revision':'a'*40,'sourceDigest':'b'*64,'dirty':False,'complete':True,'runFailures':[],'profile':'desktop',
      'environment':{'platform':'macos','hardware':'Mac test','os':'macOS test','architecture':'arm64','toolchain':'Xcode test'},
      'protocol':{'mode':'resources','runs':1,'nodes':10000,'seed':42,'samples':100,'warmup':20},'workloads':[]}
    for name in ('contacts','messages','knowledge','navigation','recommendations'):
        resources=resource_fixture(version)
        counts={p:r['samples'] for p,r in resources['phases'].items()}
        counts.update(sqliteTraversalUs=100,memoryTraversalUs=100)
        run={'platform':'macos','dataset':{'name':name,'nodes':10000,'seed':42,'edges':10000,
          'nodeStringPayloadBytes':1024 if name=='knowledge' else 512 if name=='messages' else 64,'nodePropertyCount':2,'edgePropertyCount':0},'resources':resources,
          'metrics':{p:[1.5]*n for p,n in counts.items()},'cppNewCalls':{p:[2]+[0]*(n-1) for p,n in counts.items()}}
        if version==2:
            report['protocol'].update(version=2,mutationBatchSize=250)
            run.update(protocolVersion=2,mutationBatchSize=250,mutationProtocol={
                'records':250,'nodePropertyCount':1,'scalarType':'int64','operation':'upsert-existing-node',
                'durability':'wal-sync','database':'dedicated','samples':100,'warmup':20,
                'validation':'each-commit-and-reopen'},mutationValidation={'singleCommits':120,
                'batchCommits':120,'mutations':30120,'reopenedRecords':250,'reopenVerified':True})
        report['workloads'].append({'name':name,'runs':[run]})
    return report

class ResourceMeasurementContracts(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(m,'resource diagnostics validator is missing')
        self.resource=resource_fixture()
    def test_version_two_mutation_phases_are_diagnostic_and_version_one_stays_readable(self):
        for version in (1,2):
            with self.subTest(version=version):
                self.assertEqual(m.summarize(report_fixture(version))['status'],'diagnostic-only')
                self.assertFalse(m.summarize(report_fixture(version))['hardwareQualified'])
    def test_mutation_phase_shape_counts_and_protocol_cannot_be_downgraded(self):
        edits=[lambda r:r['resources']['phases'].pop('singleMutationUs'),
               lambda r:r['resources'].update(schemaVersion=1),
               lambda r:r['metrics']['batchMutationUs'].pop(),
               lambda r:r['resources']['phases']['singleMutationUs'].update(samples=99),
               lambda r:r.update(protocolVersion=1),lambda r:r.pop('mutationBatchSize'),
               lambda r:r.update(mutationBatchSize=True),lambda r:r.update(mutationBatchSize=249),
               lambda r:r['mutationProtocol'].update(records=249),
               lambda r:r['mutationProtocol'].update(samples=8),
               lambda r:r['mutationProtocol'].update(warmup=0),
               lambda r:r['mutationProtocol'].update(nodePropertyCount=True),
               lambda r:r['mutationProtocol'].update(durability='none'),
               lambda r:r['mutationValidation'].update(singleCommits=119),
               lambda r:r['mutationValidation'].update(batchCommits=119),
               lambda r:r['mutationValidation'].update(mutations=30000),
               lambda r:r['mutationValidation'].update(reopenedRecords=249),
               lambda r:r['mutationValidation'].update(reopenVerified=False),
               lambda r:r.pop('mutationValidation')]
        for index,edit in enumerate(edits):
            report=report_fixture(2);edit(report['workloads'][0]['runs'][0])
            with self.subTest(edit=index),self.assertRaises(ValueError):m.summarize(report)
        report=report_fixture(2);report['protocol'].pop('version')
        with self.assertRaises(ValueError):m.summarize(report)
    def test_separate_workspace_probes_report_admission_failure_without_qualifying_timings(self):
        report=report_fixture(2)
        report['workloads'][0]['runs'][0]['resources']['workspaceProbes']['checkpoint']['status']='limit-exceeded'
        result=m.summarize(report)
        self.assertEqual(result['status'],'diagnostic-only')
        self.assertEqual(result['tables'][0]['workspaceProbeOutcomes']['checkpoint'],'limit-exceeded')
        edits=[lambda r:r.pop('workspaceProbes'),lambda r:r['workspaceProbes'].pop('checkpoint'),
               lambda r:r['workspaceProbes']['traversal'].update(status='ignored'),
               lambda r:r['workspaceProbes']['checkpoint'].update(api='direct'),
               lambda r:r['workspaceProbes']['checkpoint']['workspace'].update(maxLimitBytes=16777217),
               lambda r:r['workspaceProbes']['checkpoint']['workspace'].update(requests=0)]
        for edit in edits:
            resource=resource_fixture(2);edit(resource)
            with self.assertRaises(ValueError):m.validate_resources(resource)
    def test_phase_and_disk_summary_keeps_units_and_does_not_qualify(self):
        row=m.validate_resources(self.resource)
        self.assertEqual(row['maxLogicalBytes'],10)
        self.assertEqual(row['maxAllocatedBytes'],4096)
        self.assertEqual(row['maxTrackedPeakBytes'],80)
        self.assertEqual(row['maxReservedPeakBytes'],60)
    def test_invalid_peak_counts_and_missing_phases_fail_closed(self):
        for key,value in [('totalBytes',79),('peakBytes',29),('calls',True),('maxRetainedBytes',-1)]:
            r=copy.deepcopy(self.resource);r['phases']['trimUs']['allocation'][key]=value
            with self.assertRaises(ValueError):m.validate_resources(r)
        for key,value in [('maxPeakBytes',16777217),('maxCurrentBytes',61),('requests',-1)]:
            r=copy.deepcopy(self.resource);r['phases']['trimUs']['workspace'][key]=value
            with self.assertRaises(ValueError):m.validate_resources(r)
        r=copy.deepcopy(self.resource);del r['phases']['openUs']
        with self.assertRaises(ValueError):m.validate_resources(r)
    def test_disk_totals_stages_and_unavailable_measurements(self):
        for edit in (lambda r:r['diskSnapshots'].pop(),lambda r:r['diskSnapshots'][0].update(files=2),lambda r:r['diskSnapshots'][0].update(allocatedBytes=None),lambda r:r['diskSnapshots'][0]['categories']['wal'].update(logicalBytes=-1)):
            r=copy.deepcopy(self.resource);edit(r)
            with self.assertRaises(ValueError):m.validate_resources(r)
        for s in self.resource['diskSnapshots']:
            s['allocatedBytes']=None;s['allocationSupport']='unavailable'
            for c in s['categories'].values():c['allocatedBytes']=None
        self.assertIsNone(m.validate_resources(self.resource)['maxAllocatedBytes'])
    def test_instrumented_or_historical_data_cannot_be_budget_pass(self):
        r=report_fixture()
        self.assertEqual(m.summarize(r)['status'],'diagnostic-only')
        for key,value in [('complete',False),('runFailures',[{'error':'failure'}]),('dirty',None)]:
            broken=copy.deepcopy(r);broken[key]=value
            with self.assertRaises(ValueError):m.summarize(broken)
        r['workloads'][0]['runs'][0].pop('resources')
        with self.assertRaises(ValueError):m.summarize(r)
    def test_phase_samples_follow_exact_resource_protocol(self):
        for phase,expected in [('setupUs',1),('hotReadUs',100),('coldReadUs',100),('novaTraversalUs',100),('maintenanceUs',3)]:
            for count in (0,expected+1,True,float(expected)):
                with self.subTest(phase=phase,count=count):
                    r=copy.deepcopy(self.resource);r['phases'][phase]['samples']=count
                    with self.assertRaises(ValueError):m.validate_resources(r)
    def test_workspace_without_observed_contexts_is_unavailable(self):
        for phase in self.resource['phases'].values():
            phase['workspace']={k:0 for k in phase['workspace']}
        self.assertIsNone(m.validate_resources(self.resource)['maxReservedPeakBytes'])
        self.resource['phases']['trimUs']['workspace'].update(requests=1,maxLimitBytes=16777216)
        self.assertEqual(m.validate_resources(self.resource)['maxReservedPeakBytes'],0)
    def test_workspace_observation_requires_a_valid_effective_ceiling(self):
        for limit in (0,16777217,True):
            r=copy.deepcopy(self.resource);r['phases']['trimUs']['workspace'].update(maxPeakBytes=0,maxCurrentBytes=0,maxLimitBytes=limit)
            with self.subTest(limit=limit),self.assertRaises(ValueError):m.validate_resources(r)
    def test_raw_sample_counts_and_allocation_calls_match_phase_evidence(self):
        for field,edit in [('metrics',lambda d:d['hotReadUs'].pop()),('cppNewCalls',lambda d:d['maintenanceUs'].pop()),
                           ('cppNewCalls',lambda d:d['trimUs'].__setitem__(0,3)),('metrics',lambda d:d.pop('sqliteTraversalUs')),
                           ('metrics',lambda d:d.update(extraUs=[1]))]:
            r=report_fixture();edit(r['workloads'][0]['runs'][0][field])
            with self.subTest(field=field,edit=edit),self.assertRaises(ValueError):m.summarize(r)
    def test_raw_timings_and_calls_are_finite_nonnegative_numbers(self):
        for field,values in [('metrics',(-1,float('nan'),float('inf'),True,'1')),
                             ('cppNewCalls',(-1,1.5,True,None))]:
            for value in values:
                r=report_fixture();r['workloads'][0]['runs'][0][field]['coldReadUs'][0]=value
                with self.subTest(field=field,value=value),self.assertRaises(ValueError):m.summarize(r)
    def test_environment_requires_identifiable_host_and_simulator(self):
        for field,value in [('hardware',''),('os',None),('architecture',42),('toolchain','  '),('platform','ios-device')]:
            r=report_fixture();r['environment'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):m.summarize(r)
        r=report_fixture();r['environment']['platform']='ios-simulator'
        for work in r['workloads']:work['runs'][0]['platform']='ios-simulator'
        with self.assertRaises(ValueError):m.summarize(r)
        r['environment']['simulator']={'runtime':'iOS test','device':'iPhone test'}
        self.assertEqual(m.summarize(r)['status'],'diagnostic-only')
    def test_protocol_provenance_and_workload_identity_fail_closed(self):
        edits=[lambda r:r.update(schemaVersion=True),lambda r:r.update(profile='unknown'),lambda r:r.update(revision='a'*39),
               lambda r:r.update(sourceDigest='not-a-digest'),lambda r:r['protocol'].update(runs=True),
               lambda r:r['protocol'].update(samples=100.0),lambda r:r['workloads'].pop(),
               lambda r:r['workloads'].__setitem__(0,copy.deepcopy(r['workloads'][1])),
               lambda r:r['workloads'][0]['runs'].append(copy.deepcopy(r['workloads'][0]['runs'][0])),
               lambda r:r['workloads'][0]['runs'][0]['dataset'].update(nodes=10000.0),
               lambda r:r['workloads'][0]['runs'][0]['dataset'].update(seed=43),
               lambda r:r['workloads'][0]['runs'][0].update(platform='ios-simulator')]
        for index,edit in enumerate(edits):
            r=report_fixture();edit(r)
            with self.subTest(edit=index),self.assertRaises(ValueError):m.summarize(r)
        r=report_fixture();r['dirty']=True
        self.assertEqual(m.summarize(r)['status'],'diagnostic-only')
        self.assertFalse(m.summarize(r)['hardwareQualified'])
    def test_malformed_containers_raise_validation_errors(self):
        for value in (None,[],42):
            with self.subTest(value=value),self.assertRaises(ValueError):m.summarize(value)
            with self.subTest(value=value),self.assertRaises(ValueError):m.validate_resources(value)
        for field in ('environment','protocol','workloads'):
            r=report_fixture();r[field]=None
            with self.subTest(field=field),self.assertRaises(ValueError):m.summarize(r)
    def test_disk_schema_rejects_partial_or_invented_availability(self):
        edits=[lambda r:r['diskSnapshots'].reverse(),lambda r:r['diskSnapshots'][0]['categories'].pop('other'),
               lambda r:r['diskSnapshots'][0]['categories'].update(extra={'logicalBytes':0,'allocatedBytes':0,'files':0}),
               lambda r:r['diskSnapshots'][0].update(allocationSupport='unknown'),
               lambda r:r['diskSnapshots'][0].update(allocationSupport='unavailable'),
               lambda r:r['diskSnapshots'][0]['categories']['other'].update(files=True),
               lambda r:r.update(schemaVersion=True),lambda r:r.update(coverage='  ')]
        for index,edit in enumerate(edits):
            r=copy.deepcopy(self.resource);edit(r)
            with self.subTest(edit=index),self.assertRaises(ValueError):m.validate_resources(r)
        self.resource['diskSnapshots'][0]['allocatedBytes']=None
        self.resource['diskSnapshots'][0]['allocationSupport']='unavailable'
        for category in self.resource['diskSnapshots'][0]['categories'].values():category['allocatedBytes']=None
        self.assertIsNone(m.validate_resources(self.resource)['maxAllocatedBytes'])
    def test_allocation_observations_require_possible_call_and_byte_counts(self):
        for changes in ({'calls':0},{'peakBytes':0,'maxRetainedBytes':0}):
            r=copy.deepcopy(self.resource);r['phases']['trimUs']['allocation'].update(changes)
            with self.subTest(changes=changes),self.assertRaises(ValueError):m.validate_resources(r)
        self.resource['phases']['trimUs']['allocation'].update(totalBytes=0,peakBytes=0,maxRetainedBytes=0)
        m.validate_resources(self.resource)  # Successful zero-byte allocations still count as calls.
    def test_disk_file_counts_cannot_discard_nonempty_files(self):
        r=copy.deepcopy(self.resource)
        r['diskSnapshots'][0]['files']=0;r['diskSnapshots'][0]['categories']['payload']['files']=0
        with self.assertRaises(ValueError):m.validate_resources(r)
        r['diskSnapshots'][0]['logicalBytes']=0;r['diskSnapshots'][0]['categories']['payload']['logicalBytes']=0
        with self.assertRaises(ValueError):m.validate_resources(r)
        r['diskSnapshots'][0]['allocatedBytes']=0;r['diskSnapshots'][0]['categories']['payload']['allocatedBytes']=0
        m.validate_resources(r)
    def test_dataset_payload_and_edge_metadata_cannot_change_workload_shape(self):
        for name,field,value in [('contacts','edges',0),('contacts','edges',True),('contacts','edges',1.5),
                                ('contacts','nodeStringPayloadBytes',0),('messages','nodeStringPayloadBytes',64),
                                ('knowledge','nodeStringPayloadBytes',512),('contacts','nodePropertyCount',2.0),
                                ('contacts','nodePropertyCount',1),('contacts','edgePropertyCount',False),
                                ('contacts','edgePropertyCount',1)]:
            r=report_fixture();work=next(w for w in r['workloads'] if w['name']==name)
            work['runs'][0]['dataset'][field]=value
            with self.subTest(name=name,field=field,value=value),self.assertRaises(ValueError):m.summarize(r)
        for field in ('edges','nodeStringPayloadBytes','nodePropertyCount','edgePropertyCount'):
            r=report_fixture();del r['workloads'][0]['runs'][0]['dataset'][field]
            with self.subTest(missing=field),self.assertRaises(ValueError):m.summarize(r)
if __name__=='__main__':unittest.main()
