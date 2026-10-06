#!/usr/bin/env python3
"""Exercise the actual optimized Swift/C measurement executable."""
import json
import math
import subprocess
import sys

binary=sys.argv[1]
for offset in range(3):
    run=subprocess.run([binary,'--order-offset',str(offset),'--contract'],capture_output=True,text=True,timeout=120)
    assert run.returncode==0,run.stderr
    report=json.loads(run.stdout)
    assert report.get('schemaVersion')==3,'representative three-lane measurement is missing'
    assert report['validated'] is True and report['orderOffset']==offset
    assert report['warmup']==20 and report['samples']==100
    assert [s['payloadBytes'] for s in report['scenarios']]==[64,512,1024]
    for scenario in report['scenarios']:
        assert scenario['nodes']==32 and scenario['visitedNodes']==32
        for lane in ('cTransportUs','cJSONUs','swiftTypedUs'):
            values=scenario[lane]
            assert len(values)==100 and all(type(v) in (int,float) and math.isfinite(v) and v>=0 for v in values)
    assert report['operationsProtocol']['mode']=='contract'
    assert report['operationsProtocol']['samples']==2 and report['operationsProtocol']['warmup']==1
    assert [s['payloadBytes'] for s in report['operationScenarios']]==[64,512,1024]
    assert report['operationsProtocol']['mutation']=='replace-string-and-integer-properties'
    for scenario in report['operationScenarios']:
        assert scenario['coldNodes']==3 and scenario['coldEvicted']==3 and scenario['coldVisited']==3
        assert scenario['reopenValidated'] is True and scenario['batchMutations']==250
        assert scenario['traversalNodes']==32 and scenario['traversalEdges']==32 and scenario['traversalResults']==2
        for lane in ('swiftColdUs','swiftSingleMutationUs','swiftBatch250Us','swiftTraversalUs','swiftAsyncReadUs','swiftAsyncTraversalUs','swiftAsyncMutationUs'):
            assert len(scenario[lane])==2 and all(math.isfinite(v) and v>=0 for v in scenario[lane])
for arguments in (['--order-offset','3'],['--order-offset','-1'],['--order-offset'],['--unknown'],['--order-offset','0','extra'],['--contract','--contract'],['--contract','--order-offset','0']):
    run=subprocess.run([binary,*arguments],capture_output=True,text=True,timeout=10)
    assert run.returncode!=0 and not run.stdout,'invalid protocol must not produce measurement JSON'
print('Swift/C contract passed: three payload sizes, three rotating lanes, 32 records; public Swift cold/mutation/batch/traversal/async correctness, durable reopen, and invalid-input rejection.')
