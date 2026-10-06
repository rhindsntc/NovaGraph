import {createHash} from 'node:crypto';
const fail=message=>{throw new Error(message);};
const escape=value=>String(value).replaceAll('|','\\|').replaceAll('\n',' ').replaceAll('<','&lt;');
function p95(values) {
 if(!values.length||values.some(v=>typeof v!=='number'||!Number.isFinite(v)||v<0))fail('invalid performance samples');
 return [...values].sort((a,b)=>a-b)[Math.ceil(values.length*0.95)-1];
}
const nativeMetrics=['hotReadUs','coldReadUs','checkpointUs','openUs','trimUs','novaTraversalUs','singleMutationUs','batchMutationUs'];
const sampledNative=['hotReadUs','coldReadUs','novaTraversalUs','singleMutationUs','batchMutationUs'];
const exactFields=(actual,expected)=>actual&&Object.keys(actual).length===Object.keys(expected).length&&Object.entries(expected).every(([k,v])=>actual[k]===v);
function currentNativeBudgets(b){
 if(b.schemaVersion!==2){if((b.schemaVersion??1)!==1||b.review?.status!=='approved-starting-targets')fail('performance budgets lack review');return false;}
 if(b.protocolVersion!==2||b.review?.status!=='approved'||!/^\d{4}-\d{2}-\d{2}$/.test(b.review.date)||typeof b.review.source!=='string'||!b.review.source.trim())fail('performance budgets lack review');
 if(!exactFields(b.units,{timing:'microseconds',memory:'bytes',disk:'bytes',files:'count',distribution:'bytes'}))fail('invalid budget units');
 if(!Number.isInteger(b.minimumRuns)||b.minimumRuns<20||!Number.isInteger(b.minimumSamples)||b.minimumSamples<100||b.regressionFraction!==0.1)fail('invalid budget sampling policy');
 if(Object.keys(b.classes??{}).sort().join(',')!=='desktop,phone,watch')fail('missing budget classes');
 for(const [name,nodes] of Object.entries({watch:250,phone:1000,desktop:10000})){
  const row=b.classes[name];if(row.nodes!==nodes)fail('invalid budget scale');
  for(const key of [...nativeMetrics,'peakRSSBytes','diskLogicalBytes','diskAllocatedBytes','diskFiles'])if(typeof row[key]!=='number'||!Number.isFinite(row[key])||row[key]<=0||(!nativeMetrics.includes(key)&&!Number.isSafeInteger(row[key])))fail('invalid budget limit');
 }
 for(const key of ['archiveBytes','bundleLogicalBytes'])if(!Number.isSafeInteger(b.distribution?.[key])||b.distribution[key]<=0)fail('invalid distribution limit');
 return true;
}
function validateCurrentNative(r,b){
 const p=r.protocol,limit=b.classes[r.profile];
 if(!limit||p.version!==2||p.mutationBatchSize!==250||p.seed!==42||!['smoke','measure'].includes(p.mode))fail('invalid native protocol');
 for(const k of ['nodes','runs','samples','warmup'])if(!Number.isSafeInteger(p[k])||p[k]<=0)fail('invalid native sample inventory');
 if(p.mode==='measure'&&(p.nodes!==limit.nodes||p.runs<b.minimumRuns||p.samples<b.minimumSamples||p.warmup<20))fail('insufficient native measured protocol');
 if(!Array.isArray(r.runFailures)||r.runFailures.length||!Array.isArray(r.workloads)||r.workloads.map(w=>w.name).sort().join(',')!=='contacts,knowledge,messages,navigation,recommendations')fail('missing native workloads or failures');
 if(['platform','hardware','os','architecture','toolchain'].some(k=>typeof r.environment?.[k]!=='string'||!r.environment[k].trim()))fail('missing native environment');
 for(const w of r.workloads){
  if(!Array.isArray(w.runs)||w.runs.length!==p.runs)fail('native run count differs from protocol');
  for(const run of w.runs){
   const shape={records:250,nodePropertyCount:1,scalarType:'int64',operation:'upsert-existing-node',durability:'wal-sync',database:'dedicated',samples:p.samples,warmup:p.warmup,validation:'each-commit-and-reopen'};
   const count=p.samples+p.warmup;
   if(run.protocolVersion!==2||run.mutationBatchSize!==250||!exactFields(run.mutationProtocol,shape)||!exactFields(run.mutationValidation,{singleCommits:count,batchCommits:count,mutations:count*251,reopenedRecords:250,reopenVerified:true}))fail('missing native mutation evidence');
   const dataset={name:w.name,nodes:p.nodes,seed:p.seed,nodeStringPayloadBytes:w.name==='knowledge'?1024:w.name==='messages'?512:64,nodePropertyCount:2,edgePropertyCount:0};
   if(run.platform!==r.environment.platform||!run.dataset||Object.entries(dataset).some(([k,v])=>run.dataset[k]!==v)||!Number.isSafeInteger(run.dataset.edges)||run.dataset.edges<=0||Object.entries(run.dataset).some(([k,v])=>w.runs[0].dataset[k]!==v)||run.resources)fail('foreign native dataset or instrumentation');
   for(const k of nativeMetrics){const v=run.metrics?.[k];if(!Array.isArray(v)||v.length!==(sampledNative.includes(k)?p.samples:1))fail('missing native metric samples');p95(v);}
   p95([run.peakRSSBytes]);
  }
  if(r.evaluation?.status==='passed'&&(nativeMetrics.some(k=>p95(w.runs.flatMap(run=>run.metrics[k]))>limit[k])||Math.max(...w.runs.map(run=>run.peakRSSBytes))>limit.peakRSSBytes))fail('passing evaluation exceeds current budgets');
 }
 if(r.evaluation?.status==='passed'&&r.environment.platform!=={desktop:'macos',phone:'ios-device',watch:'watchos-device'}[r.profile])fail('passing evaluation has wrong hardware lane');
}
export function renderPerformanceResults(budgets,reports) {
 const current=currentNativeBudgets(budgets);
 let md='# T21 performance evidence\n\nGenerated from versioned measurement JSON and the user-approved starting budgets. **Development evidence; not a production performance guarantee.** Simulator results describe the host Mac running an iOS runtime. They do not qualify physical iPhone/watch limits.\n\n## Approved starting budgets\n\n| Class | Nodes | Hot read p95 | Cold read p95 | Checkpoint p95 | Clean open p95 | Peak process RSS |\n|---|---:|---:|---:|---:|---:|---:|\n';
 for(const [name,b] of Object.entries(budgets.classes))md+=`| ${escape(name)} | ${b.nodes} | ${b.hotReadUs/1000} ms | ${b.coldReadUs/1000} ms | ${b.checkpointUs/1000} ms | ${b.openUs/1000} ms | ${b.peakRSSBytes/1048576} MiB |\n`;
 if(current){
  md+='\n| Class | Trim p95 | Traversal p95 | Single mutation p95 | Batch 250 p95 | Logical disk | Allocated disk | Files |\n|---|---:|---:|---:|---:|---:|---:|---:|\n';
  for(const [name,b] of Object.entries(budgets.classes))md+=`| ${escape(name)} | ${b.trimUs/1000} ms | ${b.novaTraversalUs/1000} ms | ${b.singleMutationUs/1000} ms | ${b.batchMutationUs/1000} ms | ${b.diskLogicalBytes/1048576} MiB | ${b.diskAllocatedBytes/1048576} MiB | ${b.diskFiles} |\n`;
  md+=`\nComplete ZIP: ${budgets.distribution.archiveBytes/1048576} MiB. Extracted logical bundle excluding its manifest: ${budgets.distribution.bundleLogicalBytes/1048576} MiB. Disk limits apply to the maximum across seven sampled lifecycle boundaries; distribution checks require a separately verified complete artifact. Neither is a timing-report verdict. Historical reports remain historical evidence.\n`;
 }
 for(const [i,r] of reports.entries()) {
  if(typeof r.complete!=='boolean')fail('invalid completion metadata');
  if(r.schemaVersion!==1||!/^([a-f0-9]{40})$/.test(r.revision)||!/^([a-f0-9]{64})$/.test(r.sourceDigest)||typeof r.dirty!=='boolean')fail('invalid performance provenance');
  const expanded=r.protocol?.version===2,historical=current&&!expanded;
  if(expanded&&!current)fail('new native protocol requires current budgets');
  if(r.protocol?.version!==undefined&&![1,2].includes(r.protocol.version))fail('unknown native protocol');
  if(r.complete&&expanded)validateCurrentNative(r,budgets);
  const budgetFailed=r.complete && r.evaluation?.status==='failed';
  const budgetPassed=!historical && r.complete && r.evaluation?.status==='passed';
  if(budgetPassed && (r.dirty || r.protocol.mode!=='measure' || r.evaluation.hardwareQualified!==true || !Array.isArray(r.evaluation.failures) || r.evaluation.failures.length))fail('passing evaluation has inconsistent provenance or failures');
  if(budgetFailed && (!Array.isArray(r.evaluation.failures)||!r.evaluation.failures.length))fail('failed evaluation needs failure details');
  md+=`\n## Run ${i+1}: ${escape(r.environment.platform)} / ${escape(r.profile)}\n\n${!r.complete?'**FAILED — incomplete workload run**':budgetFailed?'**FAILED — performance budget checks**':budgetPassed?'**PASSED — starting performance budget checks**':'Unqualified measured development run'}. ${escape(r.protocol.mode)} protocol, ${r.protocol.nodes} nodes, ${r.protocol.runs} requested independent processes per workload, ${r.protocol.samples} point reads per process, ${r.protocol.warmup} warmups, seed ${r.protocol.seed}.\n\nRevision \`${r.revision}\`${r.dirty?' plus working tree changes':''}; source digest \`${r.sourceDigest}\`. ${escape(r.environment.hardware)}; ${escape(r.environment.os)}; ${escape(r.environment.architecture)}; ${escape(r.environment.toolchain)}.${r.environment.simulator?' Simulator '+escape(r.environment.simulator.device)+' / '+escape(r.environment.simulator.runtime)+'.':''}\n`;
  if(!r.complete) {
   if(!r.runFailures?.length)fail('incomplete report needs failure details');
   md+='\n| Workload | Failure |\n|---|---|\n';
   for(const f of r.runFailures)md+=`| ${escape(f.workload)} | ${escape(f.error)} |\n`;
   continue;
  }
  if(historical)md+='\nUnqualified for current budgets: historical protocol lacks the newly required operation evidence; any retained evaluation applies only to its original budgets.\n';
  if(budgetFailed)md+='\nBudget failures (workloads completed):\n\n'+r.evaluation.failures.map(f=>'- '+escape(f)).join('\n')+'\n';
  md+='\n| Workload | Independent runs | Hot p95 ms | Cold p95 ms | Checkpoint p95 ms | Open p95 ms | Peak RSS MiB |\n|---|---:|---:|---:|---:|---:|---:|\n';
  for(const w of r.workloads) {
   const values=key=>w.runs.flatMap(run=>run.metrics[key]);
   const metrics=['hotReadUs','coldReadUs','checkpointUs','openUs'].map(key=>(p95(values(key))/1000).toFixed(3));
   const rss=w.runs.map(run=>run.peakRSSBytes);p95(rss);
   md+=`| ${escape(w.name)} | ${w.runs.length} | ${metrics.join(' | ')} | ${(Math.max(...rss)/1048576).toFixed(2)} |\n`;
  }
  if(expanded){
   md+='\n| Workload | Trim p95 ms | Traversal p95 ms | Single mutation p95 ms | Batch 250 p95 ms |\n|---|---:|---:|---:|---:|\n';
   for(const w of r.workloads)md+=`| ${escape(w.name)} | ${['trimUs','novaTraversalUs','singleMutationUs','batchMutationUs'].map(k=>(p95(w.runs.flatMap(run=>run.metrics[k]))/1000).toFixed(3)).join(' | ')} |\n`;
   md+='\nOperation p95 values above pool samples across processes. Native scalar mutations update one integer property in a dedicated 250-record database; batches update exactly 250 records. Batch construction and correctness checks are outside timing; each commit is synchronized and acknowledged state is checked after WAL recovery without an intervening checkpoint.\n';
  }
 }
 return md+'\n## Interpretation and limits\n\nSmoke runs verify execution only. A p95 from two maintenance samples is effectively the maximum, not a stable tail estimate. Measured protocols require 20 independent processes and 100 point reads per process; all raw samples and failed runs are retained. The controlled regression gate compares both the median and p95 across independent process medians and process p95s, on identical hardware, OS, toolchain and workload protocol. Increases strictly above 10% fail. An absolute-budget result alone is not a passed regression gate; a reviewed baseline is required. P99 is descriptive, not tail qualification. No automatic baseline replacement is allowed.\n\nNative query comparisons use equivalent deterministic depth-2 reachable IDs with a 100-result bound. Nova materializes full records; SQLite and the in-memory reference traverse IDs. These are documented reference costs, not like-for-like throughput claims. SQLite uses WAL with FULL synchronous setup and a primary-key adjacency index. Reference setup and a preliminary equivalence check are outside measured intervals; timed lanes include result identity materialization and validation. Native process RSS includes dataset/reference infrastructure and is distinct from engine payload accounting.\n\nJSON retains trim/close/setup timings, ordinary C++ operator-new call counts (not C malloc, aligned allocations or Swift allocations), file IO bytes/calls, payload versus metadata/index bytes, and three overwrite/delete/maintenance snapshots. Cold reads are unique promotions from Nova cold storage; the operating-system file cache is not flushed. Native open is handle recovery in a fresh benchmark process after explicit close, not reboot/crash recovery. Separate resource diagnostics observe explicit request contexts and report untimed DSL workspace probes; direct traversal/checkpoint/close and open/recovery contexts remain unavailable. Current approved budgets also cover trim, traversal, dedicated durable scalar and 250-mutation batch timings, sampled disk/file maxima, and verified distribution sizes. Historical reports missing newly required fields remain unqualified for those checks. Swift-specific caps, linked-app size and physical-device evidence remain open. Artifact sizes remain on the Apple artifact matrix.\n';
}

// Swift diagnostics have different semantics from native budget measurements.
// Validate and aggregate their raw lanes without manufacturing a budget verdict.
export function renderSwiftResults(reports) {
 let md='';
 const lanes=['cTransportUs','cJSONUs','swiftTypedUs'];
 for(const [index,r] of reports.entries()) {
  const expanded=r.schemaVersion===2&&r.kind==='swift-diagnostic';
  if((!expanded&&(r.schemaVersion!==1||r.kind!=='swift-read-diagnostic'))||!/^([a-f0-9]{40})$/.test(r.revision)||!/^([a-f0-9]{64})$/.test(r.sourceDigest)||typeof r.dirty!=='boolean'||typeof r.complete!=='boolean'||!Array.isArray(r.runFailures))fail('invalid Swift provenance');
  const e=r.environment;
  if(!e||e.platform!=='macos'||e.configuration!=='release'||['hardware','os','architecture','toolchain'].some(k=>typeof e[k]!=='string'||!e[k].trim()))fail('invalid Swift environment');
  md+=`\n## Swift read diagnostic ${index+1}\n\n**Diagnostic only — no Swift budget or hardware qualification.** Revision \`${r.revision}\`${r.dirty?' plus working tree changes':''}; source digest \`${r.sourceDigest}\`. ${escape(e.hardware)}; ${escape(e.os)}; ${escape(e.architecture)}; ${escape(e.toolchain)}; Release.\n`;
  if(!r.complete) {
   if(!r.runFailures.length)fail('incomplete Swift report needs failures');
   md+='\n**FAILED — incomplete diagnostic**\n\n'+r.runFailures.map(f=>'- '+escape(f.error)).join('\n')+'\n';continue;
  }
  const p=r.protocol;
  if(expanded)validateSwiftOperations(r);
  if(r.runFailures.length||!p||p.runs!==20||p.samples!==100||p.warmup!==20||p.nodes!==32||JSON.stringify(p.payloadBytes)!=='[64,512,1024]'||!Array.isArray(r.runs)||r.runs.length!==20)fail('invalid Swift protocol');
  for(const [i,run] of r.runs.entries()) {
   if(run.schemaVersion!==(expanded?3:2)||run.validated!==true||run.orderOffset!==i%3||run.warmup!==20||run.samples!==100||!Array.isArray(run.scenarios)||run.scenarios.length!==3)fail('invalid Swift run');
   for(const [j,s] of run.scenarios.entries()) {
    if(s.payloadBytes!==p.payloadBytes[j]||s.nodes!==32||s.visitedNodes!==32)fail('invalid Swift scenario');
    for(const lane of lanes){if(!Array.isArray(s[lane])||s[lane].length!==100)fail('missing Swift samples');p95(s[lane]);}
   }
  }
  md+='\n20 independent processes, 32 rotating records, 20 warmups and 100 reads per lane per payload size in each process (2,000 measured reads per table lane). Each record has a string payload and an integer ordinal; all lanes validate identity, label and both values outside timing. Lane order rotates within and between processes.\n\n| Payload bytes | C transport p50 µs | C transport p95 µs | C + JSON p50 µs | C + JSON p95 µs | Typed Swift p50 µs | Typed Swift p95 µs |\n|---|---:|---:|---:|---:|---:|---:|\n';
  for(const [j,size] of p.payloadBytes.entries()) {
   const cells=lanes.flatMap(lane=>{const v=r.runs.flatMap(run=>run.scenarios[j][lane]).sort((a,b)=>a-b);return [v[Math.ceil(v.length*.5)-1].toFixed(3),p95(v).toFixed(3)];});
   md+=`| ${size} | ${cells.join(' | ')} |\n`;
  }
  md+='\nC transport includes the query, response buffer copy and free; C + JSON additionally decodes the tagged envelope with Foundation. Typed Swift includes its request lifetime, tagged-value conversion and typed decoding. These lanes expose different end-to-end costs, not pure language overhead or a like-for-like speed ratio. C lanes share one database; typed Swift uses a separate equivalent database. Setup, expected-value checks and sample bookkeeping are outside timing. The comparison reads above are hot. Swift allocation peaks and physical devices remain outside these measurements. Raw samples retain p99 data, but this is not a controlled regression baseline.\n';
  if(expanded)md+=renderSwiftOperations(r);
  else md+='\nHistorical hot-only protocol: no cold, mutation, traversal or async operation evidence.\n';
 }
 return md;
}

const operationLanes={swiftColdUs:'Cold read',swiftSingleMutationUs:'Durable scalar mutation',swiftBatch250Us:'Durable batch of 250',swiftTraversalUs:'Depth-2 traversal',swiftAsyncReadUs:'Async hot read',swiftAsyncTraversalUs:'Async traversal',swiftAsyncMutationUs:'Async durable scalar mutation'};
const operationProtocol={version:1,mode:'measure',warmup:20,samples:100,batchMutations:250,traversalNodes:32,traversalDepth:2,traversalLimit:100,graph:'directed-ring',cold:'unique-after-forced-trim',durability:'default-durable-receipt',mutation:'replace-string-and-integer-properties',asyncConcurrency:1,phaseOrder:['cold','single','batch','traversal','asyncRead','asyncTraversal','asyncMutation']};
function exactObject(value,expected){return value&&typeof value==='object'&&Object.keys(value).length===Object.keys(expected).length&&Object.entries(expected).every(([k,v])=>JSON.stringify(value[k])===JSON.stringify(v));}
function swiftProtocolDigest(value){
 const canonical=v=>Array.isArray(v)?v.map(canonical):v&&typeof v==='object'?Object.fromEntries(Object.keys(v).sort().map(k=>[k,canonical(v[k])])):v;
 return createHash('sha256').update(JSON.stringify(canonical(value))).digest('hex');
}
function validateSwiftOperations(r){
 const expected={runs:20,samples:100,warmup:20,nodes:32,payloadBytes:[64,512,1024],operations:operationProtocol};
 if(r.protocolDigest!==swiftProtocolDigest(expected)||r.protocolDigest!==swiftProtocolDigest(r.protocol))fail('invalid Swift protocol fingerprint');
 if(!exactObject(r.protocol?.operations,operationProtocol)||!Array.isArray(r.runs))fail('invalid Swift operations protocol');
 for(const run of r.runs){
  if(!exactObject(run.operationsProtocol,operationProtocol)||!Array.isArray(run.operationScenarios)||run.operationScenarios.length!==3)fail('missing Swift operations');
  for(const [i,s] of run.operationScenarios.entries()){
   const shape={payloadBytes:[64,512,1024][i],coldNodes:120,coldEvicted:120,coldVisited:120,reopenValidated:true,batchMutations:250,traversalNodes:32,traversalEdges:32,traversalResults:2};
   if(!s||Object.entries(shape).some(([k,v])=>s[k]!==v))fail('invalid Swift operation correctness');
   for(const lane of Object.keys(operationLanes)){if(!Array.isArray(s[lane])||s[lane].length!==100)fail('missing Swift operation samples');p95(s[lane]);}
  }
 }
}
function renderSwiftOperations(r){
 let md='\n### Swift operations\n\n**Diagnostic only; Swift-specific caps await approval.** Separate databases and fixed phases follow the unchanged hot comparison. Each lane has 20 warmups and 100 samples per payload per process: 2,000 observations and 20 independent process summaries. Pooled percentiles and percentiles of process summaries describe different populations. P99 remains descriptive, not tail qualification.\n\n| Payload bytes | Public Swift operation | Samples | Pooled p50 µs | Pooled p95 µs | Across-process p95 of medians µs (n=20) | Across-process p95 of p95s µs (n=20) |\n|---|---|---:|---:|---:|---:|---:|\n';
 const median=values=>[...values].sort((a,b)=>a-b)[Math.ceil(values.length*.5)-1];
 for(const [i,size] of [64,512,1024].entries())for(const [lane,label] of Object.entries(operationLanes)){
  const processes=r.runs.map(run=>run.operationScenarios[i][lane]),all=processes.flat();
  md+=`| ${size} | ${label} | ${all.length} | ${median(all).toFixed(3)} | ${p95(all).toFixed(3)} | ${p95(processes.map(median)).toFixed(3)} | ${p95(processes.map(p95)).toFixed(3)} |\n`;
 }
 return md+'\nCold reads promote 120 unique records after a forced trim with an exact eviction-count check; the OS file cache is not flushed. Single and batch writes replace both scalar properties on prepopulated records, retaining the string payload and changing the integer ordinal; transaction construction, encoding, durable commit and receipt decoding are timed. The batch contains exactly 250 mutations. Traversal uses a directed 32-node ring, outbound NEXT edges, depth 2 and limit 100, returning exactly two full records; this does not test high fanout or saturation of the result limit. Async reads, traversal and scalar mutations await the public API serially (one operation in flight), timing scheduling, operation, decoding and resumption together, not isolated queue overhead. Async mutation query text is prepared outside timing. All payloads are ASCII strings with integer ordinals. Setup, forced trim, correctness/receipt checks, repeated promotion reads and clean close/reopen checks are outside timing. Every dataset is validated after reopening. These operations are not interchangeable with native workloads or approved native numeric caps.\n';
}
