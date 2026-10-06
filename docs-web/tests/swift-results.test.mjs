import test from 'node:test';
import assert from 'node:assert/strict';
import * as performance from '../scripts/performance-results.mjs';
const fixture=()=>({schemaVersion:1,kind:'swift-read-diagnostic',revision:'a'.repeat(40),sourceDigest:'b'.repeat(64),dirty:false,complete:true,runFailures:[],protocol:{runs:20,samples:100,warmup:20,nodes:32,payloadBytes:[64,512,1024]},environment:{platform:'macos',hardware:'Mac',os:'OS',architecture:'arm64',toolchain:'Swift',configuration:'release'},runs:Array.from({length:20},(_,i)=>({schemaVersion:2,validated:true,orderOffset:i%3,warmup:20,samples:100,scenarios:[64,512,1024].map(payloadBytes=>({payloadBytes,nodes:32,visitedNodes:32,cTransportUs:Array(100).fill(1),cJSONUs:Array(100).fill(2),swiftTypedUs:Array(100).fill(3)}))}))});
function render(r){assert.equal(typeof performance.renderSwiftResults,'function','Swift evidence renderer is missing');return performance.renderSwiftResults([r]);}
test('Swift tables derive each lane from raw validated samples without a pass badge',()=>{
 const md=render(fixture());assert.match(md,/Diagnostic only/);assert.match(md,/64 \| 1\.000 \| 1\.000 \| 2\.000 \| 2\.000 \| 3\.000 \| 3\.000/);assert.doesNotMatch(md,/PASSED/);
});
test('Swift tables reject partial or corrupted complete evidence',()=>{
 for(const mutate of [r=>r.runs.pop(),r=>r.runs[0].validated=false,r=>r.runs[0].scenarios[0].cJSONUs[0]=NaN,r=>r.runs[0].scenarios[0].visitedNodes=1,r=>r.runFailures.push({error:'failure'}),r=>r.environment.configuration='debug',r=>r.protocol.nodes=1,r=>r.runs[1].orderOffset=0,r=>r.revision='bad']){
  const r=fixture();mutate(r);assert.throws(()=>render(r));
 }
});
test('Swift failure evidence stays visible with no latency table',()=>{
 const r=fixture();r.complete=false;r.runs=[];r.runFailures=[{kind:'timeout',error:'timed out'}];
 const md=render(r);assert.match(md,/FAILED/);assert.match(md,/timed out/);assert.doesNotMatch(md,/\| Payload/);
});
const expandedFixture=()=>{
 const r=fixture();r.schemaVersion=2;r.kind='swift-diagnostic';r.protocolDigest='b5d9550d26d581d8da1b79efd71a8ab9095f66313839f78e126c5626d8913d4a';
 r.protocol.operations={version:1,mode:'measure',warmup:20,samples:100,batchMutations:250,traversalNodes:32,traversalDepth:2,traversalLimit:100,graph:'directed-ring',cold:'unique-after-forced-trim',durability:'default-durable-receipt',mutation:'replace-string-and-integer-properties',asyncConcurrency:1,phaseOrder:['cold','single','batch','traversal','asyncRead','asyncTraversal','asyncMutation']};
 for(const run of r.runs){run.schemaVersion=3;run.operationsProtocol=structuredClone(r.protocol.operations);run.operationScenarios=[64,512,1024].map(payloadBytes=>({payloadBytes,coldNodes:120,coldEvicted:120,coldVisited:120,reopenValidated:true,batchMutations:250,traversalNodes:32,traversalEdges:32,traversalResults:2,...Object.fromEntries(['swiftColdUs','swiftSingleMutationUs','swiftBatch250Us','swiftTraversalUs','swiftAsyncReadUs','swiftAsyncTraversalUs','swiftAsyncMutationUs'].map(lane=>[lane,Array(100).fill(4)]))}));}
 return r;
};
test('expanded Swift diagnostics show operation samples and independent processes without native caps',()=>{
 const md=render(expandedFixture());assert.match(md,/Swift operations/);assert.match(md,/2,000/);assert.match(md,/20 independent/);assert.match(md,/batch of 250/);assert.match(md,/OS file cache/);assert.doesNotMatch(md,/PASSED|All reads are hot/);
});
test('expanded Swift evidence rejects incomplete cold coverage, contract counts and absent durable validation',()=>{
 for(const mutate of [r=>r.protocolDigest='0'.repeat(64),r=>r.runs[0].schemaVersion=2,r=>r.runs[0].operationScenarios[0].coldEvicted=119,r=>r.runs[0].operationScenarios[0].reopenValidated=false,r=>r.runs[0].operationScenarios[0].swiftAsyncReadUs.pop(),r=>r.runs[0].operationScenarios[0].swiftAsyncReadUs[0]=Infinity,r=>r.runs[0].operationsProtocol.mode='contract',r=>r.protocol.operations.batchMutations=249]){const r=expandedFixture();mutate(r);assert.throws(()=>render(r));}
});
