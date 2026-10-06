import test from 'node:test';
import assert from 'node:assert/strict';
import {renderPerformanceResults} from '../scripts/performance-results.mjs';
const budgets={review:{status:'approved-starting-targets'},classes:{phone:{nodes:1000,hotReadUs:5000,coldReadUs:20000,checkpointUs:1000000,openUs:1000000,peakRSSBytes:134217728}}};
const report={schemaVersion:1,revision:'a'.repeat(40),sourceDigest:'b'.repeat(64),dirty:true,profile:'phone',environment:{platform:'ios-simulator',hardware:'Mac',os:'test',toolchain:'Xcode test',architecture:'arm64'},protocol:{mode:'smoke',nodes:32,runs:2,samples:8,warmup:2,seed:42},complete:true,workloads:[{name:'contacts',runs:[{metrics:{hotReadUs:[1,2],coldReadUs:[3,4],checkpointUs:[5],openUs:[6]},peakRSSBytes:1000}]}]};
test('simulator smoke evidence stays explicitly unqualified and values come from samples',()=>{
 const md=renderPerformanceResults(budgets,[report]);assert.match(md,/Unqualified/);assert.match(md,/ios-simulator/);assert.match(md,/working tree/);assert.match(md,/0\.002/);assert.match(md,/32 nodes/);
});
test('failed workloads remain visible instead of becoming empty success tables',()=>{
 const md=renderPerformanceResults(budgets,[{...report,complete:false,runFailures:[{workload:'contacts',error:'query working memory budget exceeded'}],workloads:[]}]);assert.match(md,/FAILED/);assert.match(md,/query working memory budget exceeded/);
});
test('invalid samples and provenance fail documentation generation',()=>{
 assert.throws(()=>renderPerformanceResults(budgets,[{...report,revision:'missing'}]));
 const bad=structuredClone(report);bad.workloads[0].runs[0].metrics.hotReadUs=[NaN];assert.throws(()=>renderPerformanceResults(budgets,[bad]));
});
test('completed runs retain budget failures alongside measured samples',()=>{
 const failed={...report,evaluation:{status:'failed',failures:['contacts checkpointUs p95 1250000 > 1000000']}};
 const md=renderPerformanceResults(budgets,[failed]);
 assert.match(md,/FAILED — performance budget checks/);
 assert.match(md,/contacts checkpointUs p95 1250000 > 1000000/);
 assert.match(md,/Independent runs/);assert.match(md,/0\.002/);
});
test('failed performance evaluation requires visible failure details',()=>{
 assert.throws(()=>renderPerformanceResults(budgets,[{...report,evaluation:{status:'failed',failures:[]}}]),/failure details/);
});
test('passing measured budgets are visible without claiming production readiness',()=>{
 const passed={...report,dirty:false,environment:{...report.environment,platform:'macos'},protocol:{...report.protocol,mode:'measure'},evaluation:{status:'passed',hardwareQualified:true,failures:[]}};
 const md=renderPerformanceResults(budgets,[passed]);
 assert.match(md,/PASSED — starting performance budget checks/);
 assert.match(md,/not a production performance guarantee/);
 assert.match(md,/Independent runs/);
});
test('passing evaluation cannot hide failures or unqualified provenance',()=>{
 const passed={...report,dirty:false,protocol:{...report.protocol,mode:'measure'},evaluation:{status:'passed',hardwareQualified:true,failures:[]}};
 for(const bad of [
  {...passed,dirty:true},
  {...passed,protocol:{...passed.protocol,mode:'smoke'}},
  {...passed,evaluation:{...passed.evaluation,failures:['budget exceeded']}},
  {...passed,evaluation:{...passed.evaluation,hardwareQualified:false}},
 ])assert.throws(()=>renderPerformanceResults(budgets,[bad]),/passing evaluation/);
});

const currentBudgets=JSON.parse((await import('node:fs')).readFileSync(new URL('../../benchmarks/budgets.json',import.meta.url),'utf8'));
function currentReport(){
 const r=structuredClone(report);r.protocol.version=2;r.protocol.mutationBatchSize=250;r.runFailures=[];
 r.workloads=['contacts','messages','knowledge','navigation','recommendations'].map(name=>({name,runs:Array.from({length:2},()=>({
  platform:'ios-simulator',protocolVersion:2,mutationBatchSize:250,
  dataset:{name,nodes:32,seed:42,edges:1,nodeStringPayloadBytes:name==='knowledge'?1024:name==='messages'?512:64,nodePropertyCount:2,edgePropertyCount:0},
  mutationProtocol:{records:250,nodePropertyCount:1,scalarType:'int64',operation:'upsert-existing-node',durability:'wal-sync',database:'dedicated',samples:8,warmup:2,validation:'each-commit-and-reopen'},
  mutationValidation:{singleCommits:10,batchCommits:10,mutations:2510,reopenedRecords:250,reopenVerified:true},
  metrics:Object.fromEntries(['hotReadUs','coldReadUs','novaTraversalUs','singleMutationUs','batchMutationUs','checkpointUs','openUs','trimUs','setupUs','closeUs','sqliteTraversalUs','memoryTraversalUs'].map(k=>[k,Array(['hotReadUs','coldReadUs','novaTraversalUs','singleMutationUs','batchMutationUs','sqliteTraversalUs','memoryTraversalUs'].includes(k)?8:1).fill(1000)])),peakRSSBytes:1000
 }))}));return r;
}
test('current budgets render added native operations and footprint caps',()=>{
 const md=renderPerformanceResults(currentBudgets,[currentReport()]);
 for(const text of ['Trim p95','Traversal p95','Single mutation p95','Batch 250 p95','Logical disk','Allocated disk','Complete ZIP','Historical','process medians','process p95s'])assert.ok(md.includes(text),text);
 assert.match(md,/1\.000/);assert.match(md,/Unqualified/);
});
test('historical passed evidence cannot qualify current budgets',()=>{
 const old={...report,dirty:false,protocol:{...report.protocol,mode:'measure'},evaluation:{status:'passed',hardwareQualified:true,failures:[]}};
 const md=renderPerformanceResults(currentBudgets,[old]);
 assert.doesNotMatch(md,/PASSED/);assert.match(md,/historical protocol/i);
});
test('current native renderer rejects missing protocol operation evidence and malformed budgets',()=>{
 for(const edit of [r=>delete r.protocol.mutationBatchSize,r=>delete r.workloads[0].runs[0].metrics.singleMutationUs,r=>r.workloads[0].runs[0].metrics.batchMutationUs.pop(),r=>delete r.workloads[0].runs[0].mutationValidation,r=>r.workloads[0].runs[0].dataset.nodes=16]){
  const r=currentReport();edit(r);assert.throws(()=>renderPerformanceResults(currentBudgets,[r]));
 }
 for(const edit of [b=>delete b.classes.phone.singleMutationUs,b=>b.review.status='proposed',b=>b.units.timing='seconds',b=>delete b.distribution]){
  const b=structuredClone(currentBudgets);edit(b);assert.throws(()=>renderPerformanceResults(b,[currentReport()]));
 }
});

test('malformed completion metadata cannot render as accepted evidence',()=>{
 for(const complete of ['false','true',1,0,null,undefined]){
  assert.throws(()=>renderPerformanceResults(currentBudgets,[{...currentReport(),complete}]),/completion/);
  assert.throws(()=>renderPerformanceResults(budgets,[{...report,complete}]),/completion/);
 }
});
