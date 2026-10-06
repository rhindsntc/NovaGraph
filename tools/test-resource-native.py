#!/usr/bin/env python3
"""Exercise actual instrumented native workloads; report construction must not be metered."""
import json
import subprocess
import sys
from resource_measurements import validate_resources, WORKLOADS

for name in WORKLOADS:
    result=subprocess.run([sys.argv[1],name,'32','42','100','20'],capture_output=True,text=True,timeout=120)
    if result.returncode:raise AssertionError(result.stderr)
    report=json.loads(result.stdout);resource=report['resources'];validate_resources(resource)
    assert resource['schemaVersion']==2 and report['protocolVersion']==2
    for phase in ('setupUs','hotReadUs','coldReadUs','trimUs','maintenanceUs',
                  'singleMutationUs','batchMutationUs'):
        assert resource['phases'][phase]['workspace']['requests']>0, phase
    for phase in ('novaTraversalUs','checkpointUs','closeUs','openUs'):
        assert resource['phases'][phase]['workspace']['requests']==0, phase
    probes=resource['workspaceProbes']
    assert set(probes)=={'traversal','checkpoint'}
    for probe in probes.values():
        assert probe['status']=='observed' and probe['workspace']['requests']==1
        assert 0<probe['workspace']['maxPeakBytes']<=16777216
    assert report['mutationValidation']=={'singleCommits':120,'batchCommits':120,'mutations':30120,
        'reopenedRecords':250,'reopenVerified':True}
    for phase in ('singleMutationUs','batchMutationUs'):
        assert len(report['metrics'][phase])==100
        assert resource['phases'][phase]['workspace']['requests']==100
    for phase,row in resource['phases'].items():
        assert row['samples']==len(report['metrics'][phase]), phase
        assert row['allocation']['calls']==sum(report['cppNewCalls'][phase]), phase
        assert row['allocation']['peakBytes']>0, phase
    assert resource['phases']['closeUs']['allocation']['maxRetainedBytes']==0
    snapshots={r['stage']:r for r in resource['diskSnapshots']}
    assert snapshots['final-checkpoint']['logicalBytes']==report['disk']['bytes']
    assert snapshots['final-checkpoint']['files']==report['disk']['files']
    assert snapshots['after-close']['categories']==snapshots['after-reopen']['categories']
    for cycle in range(3):
        assert snapshots[f'maintenance-{cycle}']['logicalBytes']==report['maintenanceTrend'][cycle]['bytes']
    assert all(s['categories']['payload']['files']>0 and s['categories']['wal']['files']>0 and s['categories']['catalog']['files']>0 for s in snapshots.values())
    print(f'{name}: native resource phase/lifecycle contracts passed')

# A real bounded observation may reject a large checkpoint without invalidating
# the direct-API diagnostic run or silently increasing its workspace ceiling.
result=subprocess.run([sys.argv[1],'knowledge','2200','42','8','2'],capture_output=True,text=True,timeout=120)
assert result.returncode==0,result.stderr
large=json.loads(result.stdout)
probe=large['resources']['workspaceProbes']['checkpoint']
assert probe['status']=='limit-exceeded',probe
assert probe['workspace']['maxLimitBytes']==16777216
assert large['mutationValidation']['reopenVerified'] is True
print('Large graph: bounded checkpoint observation rejection remains explicit and nonfatal')
