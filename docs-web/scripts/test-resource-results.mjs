import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {renderResourceResults as render} from './resource-results.mjs';
const phases=['setupUs','hotReadUs','coldReadUs','novaTraversalUs','trimUs','maintenanceUs','checkpointUs','closeUs','openUs'];
const samples=p=>['hotReadUs','coldReadUs','novaTraversalUs','sqliteTraversalUs','memoryTraversalUs'].includes(p)?100:p==='maintenanceUs'?3:1;
const metrics=[...phases,'sqliteTraversalUs','memoryTraversalUs'];
const stages=['after-trim','maintenance-0','maintenance-1','maintenance-2','final-checkpoint','after-close','after-reopen'];
const workloads=['contacts','messages','knowledge','navigation','recommendations'];
function resources(){return {schemaVersion:1,instrumented:true,coverage:'C++ new; explicit request contexts; regular-file snapshots',phases:Object.fromEntries(phases.map(p=>[p,{samples:samples(p),allocation:{calls:2*samples(p),totalBytes:100,peakBytes:80,maxRetainedBytes:30},workspace:{requests:1,maxPeakBytes:60,maxCurrentBytes:10,maxLimitBytes:16777216}}])),diskSnapshots:stages.map(stage=>({stage,logicalBytes:10,allocatedBytes:4096,files:1,allocationSupport:'st_blocks_512',categories:{payload:{logicalBytes:10,allocatedBytes:4096,files:1},catalog:{logicalBytes:0,allocatedBytes:0,files:0},wal:{logicalBytes:0,allocatedBytes:0,files:0},other:{logicalBytes:0,allocatedBytes:0,files:0}}}))};}
function fixture(){return {entry:{id:'desktop-resources',title:'Desktop resources'},report:{schemaVersion:1,revision:'a'.repeat(40),sourceDigest:'b'.repeat(64),dirty:false,complete:true,runFailures:[],profile:'desktop',environment:{platform:'macos',hardware:'Mac',os:'macOS',architecture:'arm64',toolchain:'clang'},protocol:{mode:'resources',runs:1,nodes:10000,seed:42,samples:100,warmup:20},workloads:workloads.map(name=>({name,runs:[{platform:'macos',dataset:{name,nodes:10000,seed:42,edges:1,nodeStringPayloadBytes:name==='knowledge'?1024:name==='messages'?512:64,nodePropertyCount:2,edgePropertyCount:0},resources:resources(),metrics:Object.fromEntries(metrics.map(p=>[p,Array(samples(p)).fill(1)])),cppNewCalls:Object.fromEntries(metrics.map(p=>[p,Array(samples(p)).fill(2)]))}]}))}};}
const scope='Logical regular-file bytes in the verified extracted bundle, excluding artifact-manifest.json; fileCount uses the same exclusion. archiveBytes is the complete verified ZIP including artifact-manifest.json. Slice sizes are native static archives and independent ZIP/DEFLATE archives, not linked or installed application size. No runtime qualification is implied.';
function artifact(){
 const ids=['macos','ios','ios-simulator','catalyst','tvos','tvos-simulator','watchos','watchos-simulator'];
 const files=Object.fromEntries(ids.map((id,i)=>[id+'/library.a',String(i+1).repeat(64)]));
 // Python json.dumps(sort_keys=True) default separators, matching the producer.
 const serialized='{'+Object.keys(files).sort().map(k=>JSON.stringify(k)+': '+JSON.stringify(files[k])).join(', ')+'}';
 const digest=createHash('sha256').update(serialized).digest('hex');
 const slices=ids.map((id,i)=>({id,installedBytes:12,compressedBytes:130,sha256:String(i+1).repeat(64)}));
 return {schemaVersion:1,revision:'c'.repeat(40),dirty:false,artifactContentDigest:digest,files,slices,archive:{name:'NovaGraph.zip',sha256:'d'.repeat(64),bytes:2048},qualification:{releaseReady:false,blockers:['runtime unavailable']},resourceSizes:{schemaVersion:1,revision:'c'.repeat(40),dirty:false,artifactContentDigest:digest,bundleLogicalBytes:96,archiveBytes:2048,archiveSha256:'d'.repeat(64),fileCount:8,scope,files:Object.fromEntries(Object.entries(files).map(([name,sha256])=>[name,{bytes:12,sha256}])),slices:structuredClone(slices)}};
}
test('resource tables render phase and disk units with diagnostic limits',()=>{
 const md=render([fixture()]);
 for(const expected of ['Desktop resources','contacts','knowledge','Diagnostic only','C malloc','Swift','not heap','not a transient peak','Tracked peak bytes','Tracked retained bytes','Tracked total bytes','Allocation calls','Reserved peak bytes','Reserved current bytes','Reservation limit bytes','Logical bytes','Allocated bytes','Files','after-close','after-reopen','payload','catalog','wal','other'])assert.ok(md.includes(expected),expected);
 assert.match(md,/\| contacts \| 80 \| 60 \| 10 \| 4096 \| 1 \|/);
 assert.match(md,/\| trim \| 1 \| 80 \| 30 \| 100 \| 2 \| 1 \| 60 \| 10 \| 16777216 \|/);
 assert.match(md,/\| after-trim \| payload \| 10 \| 4096 \| 1 \|/);
 assert.doesNotMatch(md,/PASSED|hardware qualified/i);
});
test('resource renderer rejects missing phases and invalid allocation or reservation relationships',()=>{
 const edits=[r=>delete r.phases.openUs,r=>r.instrumented=false,r=>r.schemaVersion=2,r=>r.coverage='',r=>r.phases.trimUs.allocation.peakBytes=101,r=>r.phases.trimUs.allocation.maxRetainedBytes=81,r=>r.phases.trimUs.allocation.calls=true,r=>r.phases.trimUs.workspace.maxCurrentBytes=61,r=>r.phases.trimUs.workspace.maxPeakBytes=16777217,r=>r.phases.trimUs.workspace.requests=0,r=>r.phases.trimUs.samples=0,r=>r.phases.trimUs.allocation.totalBytes=Number.MAX_SAFE_INTEGER+1];
 for(const edit of edits){const f=fixture();edit(f.report.workloads[0].runs[0].resources);assert.throws(()=>render([f]));}
});
test('resource renderer rejects missing lifecycle snapshots and inconsistent disk categories',()=>{
 const edits=[r=>r.diskSnapshots.pop(),r=>r.diskSnapshots.reverse(),r=>delete r.diskSnapshots[0].categories.wal,r=>r.diskSnapshots[0].files=2,r=>r.diskSnapshots[0].allocatedBytes=null,r=>r.diskSnapshots[0].categories.payload.logicalBytes=-1,r=>r.diskSnapshots[0].allocationSupport='guess'];
 for(const edit of edits){const f=fixture();edit(f.report.workloads[0].runs[0].resources);assert.throws(()=>render([f]));}
});
test('unavailable allocated disk bytes stay unavailable instead of zero',()=>{
 const f=fixture();for(const w of f.report.workloads)for(const s of w.runs[0].resources.diskSnapshots){s.allocationSupport='unavailable';s.allocatedBytes=null;for(const c of Object.values(s.categories))c.allocatedBytes=null;}
 const md=render([f]);assert.match(md,/\| contacts \| 80 \| 60 \| 10 \| Unavailable \| 1 \|/);assert.match(md,/\| after-close \| total \| 10 \| Unavailable \| 1 \|/);
 f.report.workloads[0].runs[0].resources.diskSnapshots[0].categories.wal.allocatedBytes=0;assert.throws(()=>render([f]));
});
test('resource reports reject incomplete foreign or timing evidence',()=>{
 const edits=[r=>r.complete=false,r=>r.runFailures=[{error:'failed'}],r=>r.revision='unknown',r=>r.sourceDigest='bad',r=>r.dirty=null,r=>r.protocol.mode='measure',r=>r.protocol.runs=20,r=>r.workloads.pop(),r=>r.workloads[0].runs.push(r.workloads[0].runs[0]),r=>r.workloads[0].runs[0].platform='ios-simulator',r=>r.workloads[0].runs[0].dataset.name='other',r=>r.evaluation={status:'passed',hardwareQualified:true,failures:[]}];
 for(const edit of edits){const f=fixture();edit(f.report);assert.throws(()=>render([f]));}
});
test('resource report entries escape markup and retain dirty diagnostic provenance',()=>{
 const f=fixture();f.entry.title='Resources | <script>\nunsafe';f.report.dirty=true;
 const md=render([f]);assert.match(md,/Resources \\\| &lt;script&gt; unsafe/);assert.match(md,/working tree changes/);
 assert.throws(()=>render([{entry:{id:'a'},report:f.report}]));
 assert.throws(()=>render([f,{...f}]));
});
test('fresh artifact sizes render independently of consumer runtime qualification',()=>{
 const a=artifact(),before=structuredClone(a),md=render([],a);
 for(const text of ['Fresh artifact sizes','96','2048','Native archive bytes','Independent ZIP bytes','130','artifact-manifest.json','runtime qualification','installed application size'])assert.ok(md.includes(text),text);
 assert.match(md,/cccccccccccccccccccccccccccccccccccccccc/);assert.deepEqual(a,before);assert.doesNotMatch(md,/release ready|minimum-runtime checks passed/i);
});
test('artifact renderer rejects absent stale dirty or foreign size evidence',()=>{
 const edits=[a=>delete a.resourceSizes,a=>a.dirty=true,a=>a.resourceSizes.dirty=true,a=>a.resourceSizes.revision='a'.repeat(40),a=>a.resourceSizes.artifactContentDigest='f'.repeat(64),a=>a.artifactContentDigest='f'.repeat(64),a=>a.files['new']='f'.repeat(64),a=>a.resourceSizes.schemaVersion=2,a=>a.resourceSizes.scope='app size'];
 for(const edit of edits){const a=artifact();edit(a);assert.throws(()=>render([],a));}
});
test('artifact renderer rejects missing counters hash inventory and inconsistent per-slice sizes',()=>{
 const edits=[a=>a.resourceSizes.bundleLogicalBytes=97,a=>a.resourceSizes.fileCount=9,a=>a.resourceSizes.archiveBytes=100,a=>a.resourceSizes.archiveSha256='e'.repeat(64),a=>delete a.resourceSizes.files['ios/library.a'],a=>a.resourceSizes.files['ios/library.a'].sha256='e'.repeat(64),a=>a.resourceSizes.files['ios/library.a'].bytes=true,a=>a.resourceSizes.slices[0].installedBytes=11,a=>a.resourceSizes.slices.pop(),a=>a.slices.pop(),a=>a.slices[0].compressedBytes=0,a=>delete a.archive,a=>a.resourceSizes.fileCount=Number.MAX_SAFE_INTEGER+1];
 for(const edit of edits){const a=artifact();edit(a);assert.throws(()=>render([],a));}
});
test('resource renderer does not invent missing historical measurements',()=>{
 const md=render([]);assert.match(md,/No resource diagnostic reports/);assert.match(md,/Artifact size refresh unavailable/);
});

test('raw metrics and allocation samples must agree with protocol and phase totals',()=>{
 const edits=[run=>delete run.metrics,run=>delete run.cppNewCalls,run=>run.metrics.hotReadUs.pop(),run=>run.cppNewCalls.trimUs[0]=3,run=>run.metrics.openUs[0]=Infinity,run=>run.cppNewCalls.openUs[0]=true,run=>run.resources.phases.hotReadUs.samples=1,run=>run.resources.phases.trimUs.workspace.maxLimitBytes=33554432];
 for(const edit of edits){const f=fixture();edit(f.report.workloads[0].runs[0]);assert.throws(()=>render([f]));}
});
test('environment and simulator provenance must be complete',()=>{
 const f=fixture();delete f.report.environment.toolchain;assert.throws(()=>render([f]));
 const simulator=fixture();simulator.report.environment.platform='ios-simulator';for(const w of simulator.report.workloads)w.runs[0].platform='ios-simulator';
 assert.throws(()=>render([simulator]));simulator.report.environment.simulator={device:'iPhone test',runtime:'27.0'};
 assert.match(render([simulator]),/iPhone test/);
});
test('unobserved workspace reservations are unavailable while observed zero usage is zero',()=>{
 const f=fixture();for(const w of f.report.workloads)for(const p of Object.values(w.runs[0].resources.phases))p.workspace={requests:0,maxPeakBytes:0,maxCurrentBytes:0,maxLimitBytes:0};
 assert.match(render([f]),/\| contacts \| 80 \| Unavailable \| 10 \| 4096 \| 1 \|/);
 f.report.workloads[0].runs[0].resources.phases.trimUs.workspace={requests:1,maxPeakBytes:0,maxCurrentBytes:0,maxLimitBytes:16777216};
 assert.match(render([f]),/\| contacts \| 80 \| 0 \| 10 \| 4096 \| 1 \|/);
});

test('resource byte totals require corresponding allocation calls and disk files',()=>{
 const allocation=fixture(),run=allocation.report.workloads[0].runs[0];
 run.resources.phases.trimUs.allocation.calls=0;run.cppNewCalls.trimUs=[0];
 assert.throws(()=>render([allocation]));
 const disk=fixture(),snapshot=disk.report.workloads[0].runs[0].resources.diskSnapshots[0];
 snapshot.files=0;snapshot.categories.payload.files=0;
 assert.throws(()=>render([disk]));
});

test('resource dataset shape must match the workload protocol',()=>{
 for(const [field,value] of [['edges',0],['nodeStringPayloadBytes',1024],['nodePropertyCount',3],['edgePropertyCount',1]]){const f=fixture();f.report.workloads[0].runs[0].dataset[field]=value;assert.throws(()=>render([f]));}
});

function fixtureV2(){
 const f=fixture();f.report.protocol={...f.report.protocol,version:2,mutationBatchSize:250};
 for(const work of f.report.workloads){
  const run=work.runs[0];run.protocolVersion=2;run.mutationBatchSize=250;
  run.mutationProtocol={records:250,nodePropertyCount:1,scalarType:'int64',operation:'upsert-existing-node',durability:'wal-sync',database:'dedicated',samples:100,warmup:20,validation:'each-commit-and-reopen'};
  run.mutationValidation={singleCommits:120,batchCommits:120,mutations:30120,reopenedRecords:250,reopenVerified:true};
  run.resources.schemaVersion=2;
  for(const name of ['singleMutationUs','batchMutationUs']){
   run.resources.phases[name]=structuredClone(run.resources.phases.hotReadUs);
   run.metrics[name]=Array(100).fill(1);run.cppNewCalls[name]=Array(100).fill(2);
  }
  run.resources.workspaceProbes=Object.fromEntries(['traversal','checkpoint'].map(name=>[name,{api:'execute_dsl',status:name==='checkpoint'?'limit-exceeded':'observed',workspace:{requests:1,maxPeakBytes:90,maxCurrentBytes:0,maxLimitBytes:16777216}}]));
 }
 return f;
}
test('resource v2 renders mutation phases and separate workspace probe admission failures',()=>{
 const f=fixtureV2(),md=render([f]);
 for(const expected of ['protocol version 2','singleMutation','batchMutation','Separate workspace probes','limit-exceeded','execute_dsl','direct traversal/checkpoint/close','90'])assert.ok(md.includes(expected),expected);
 assert.match(md,/\| contacts \| 80 \| 60 \| 10 \| 4096 \| 1 \|/);
 assert.doesNotMatch(md,/PASSED|hardware qualified/i);
});
test('resource v2 rejects mismatched mutation and workspace probe evidence',()=>{
 const edits=[f=>f.report.protocol.mutationBatchSize=249,f=>f.report.workloads[0].runs[0].protocolVersion=1,
 f=>f.report.workloads[0].runs[0].mutationProtocol.records=1,f=>f.report.workloads[0].runs[0].mutationValidation.reopenVerified=false,
 f=>delete f.report.workloads[0].runs[0].resources.workspaceProbes.checkpoint,
 f=>f.report.workloads[0].runs[0].resources.workspaceProbes.traversal.status='passed',
 f=>f.report.workloads[0].runs[0].resources.workspaceProbes.traversal.workspace.maxPeakBytes=16777217,
 f=>f.report.workloads[0].runs[0].metrics.singleMutationUs.pop(),
 f=>f.report.workloads[0].runs[0].resources.phases.singleMutationUs.samples=1];
 for(const edit of edits){const f=fixtureV2();edit(f);assert.throws(()=>render([f]));}
});
