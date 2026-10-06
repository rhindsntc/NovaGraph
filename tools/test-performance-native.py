#!/usr/bin/env python3
"""Validate real native workload output; correctness comparisons execute inside it."""
import json,subprocess,sys
binary=sys.argv[1]
expected={'contacts':92,'messages':61,'knowledge':160,'navigation':31,'recommendations':186}
for name,edges in expected.items():
    r=subprocess.run([binary,name,'32','42','8','2'],capture_output=True,text=True,timeout=120)
    assert r.returncode==0,r.stderr
    j=json.loads(r.stdout)
    assert j['protocolVersion']==2 and j['mutationBatchSize']==250
    assert j['mutationProtocol']=={'records':250,'nodePropertyCount':1,'scalarType':'int64',
        'operation':'upsert-existing-node','durability':'wal-sync','database':'dedicated',
        'samples':8,'warmup':2,'validation':'each-commit-and-reopen'}
    assert j['mutationValidation']=={'singleCommits':10,'batchCommits':10,'mutations':2510,
        'reopenedRecords':250,'reopenVerified':True}
    assert j['mutationIO']['fileSyncMembers']>=20
    for metric in ('singleMutationUs','batchMutationUs'):
        assert len(j['metrics'][metric])==8 and len(j['cppNewCalls'][metric])==8
    assert j['dataset']['edges']==edges and j['dataset']['nodes']==32
    assert j['memoryBeforeTrim']['hotPayloadBytes']>0 and j['memoryAfterTrim']['hotPayloadBytes']==0
    assert j['coldPromotionReadBytes']>0 and j['peakRSSBytes']>0
    assert len(j['metrics']['hotReadUs'])==8 and len(j['metrics']['coldReadUs'])==8
    for key in ('trimUs','checkpointUs','openUs','closeUs'):assert len(j['metrics'][key])==1
    assert j['maintenanceTrend'][-1]==j['maintenanceTrend'][-2],j['maintenanceTrend']
    for values in j['metrics'].values():assert all(v>=0 for v in values)
# More than one default 4096-record trim page is needed; the harness must drain it.
r=subprocess.run([binary,'navigation','2200','42','8','2'],capture_output=True,text=True,timeout=120)
assert r.returncode==0,r.stderr
large=json.loads(r.stdout)
assert large['evictedRecords']==4399 and large['memoryAfterTrim']['hotPayloadBytes']==0
for args in ([],['bad','32','42','8','2'],['contacts','0','42','8','2'],['contacts','32','42','0','2']):
    r=subprocess.run([binary,*args],capture_output=True,text=True,timeout=10)
    assert r.returncode==1 and 'Performance run failed' in r.stderr
print('Native contracts: five seeded graph/trim/maintenance workloads and four invalid inputs passed.')
