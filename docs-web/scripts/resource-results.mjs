import {createHash} from 'node:crypto';

const PHASES=['setupUs','hotReadUs','coldReadUs','novaTraversalUs','trimUs','maintenanceUs','checkpointUs','closeUs','openUs'];
const PHASES_V2=[...PHASES,'singleMutationUs','batchMutationUs'];
const phaseSamples=name=>['singleMutationUs','batchMutationUs','hotReadUs','coldReadUs','novaTraversalUs','sqliteTraversalUs','memoryTraversalUs'].includes(name)?100:name==='maintenanceUs'?3:1;
const STAGES=['after-trim','maintenance-0','maintenance-1','maintenance-2','final-checkpoint','after-close','after-reopen'];
const WORKLOADS=['contacts','messages','knowledge','navigation','recommendations'];
const CATEGORIES=['payload','catalog','wal','other'];
const SLICES=['macos','ios','ios-simulator','catalyst','tvos','tvos-simulator','watchos','watchos-simulator'];
const SIZE_SCOPE='Logical regular-file bytes in the verified extracted bundle, excluding artifact-manifest.json; fileCount uses the same exclusion. archiveBytes is the complete verified ZIP including artifact-manifest.json. Slice sizes are native static archives and independent ZIP/DEFLATE archives, not linked or installed application size. No runtime qualification is implied.';
const fail=message=>{throw new Error(message);};
const object=value=>value!==null&&typeof value==='object'&&!Array.isArray(value);
const text=value=>typeof value==='string'&&value.trim().length>0;
const escape=value=>String(value).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('|','\\|').replaceAll('\n',' ').replaceAll('\r',' ');
const hex=(value,length)=>typeof value==='string'&&new RegExp(`^[a-f0-9]{${length}}$`).test(value);
const integer=(value,min=0)=>Number.isSafeInteger(value)&&value>=min?value:fail('invalid resource counter');
const sameKeys=(value,keys)=>object(value)&&Object.keys(value).length===keys.length&&keys.every(k=>Object.hasOwn(value,k));
const sum=values=>values.reduce((total,value)=>integer(total+integer(value)),0);
const shown=value=>value===null?'Unavailable':String(value);
const table=(headers,rows)=>'\n| '+headers.join(' | ')+' |\n| '+headers.map(()=>'---').join(' | ')+' |\n'+rows.map(row=>'| '+row.map(escape).join(' | ')+' |').join('\n')+'\n';

function validateResources(resource) {
 const phasesExpected=resource?.schemaVersion===2?PHASES_V2:PHASES;
 if(!object(resource)||![1,2].includes(resource.schemaVersion)||resource.instrumented!==true||!text(resource.coverage)||!sameKeys(resource.phases,phasesExpected))fail('invalid resource schema or missing phases');
 for(const [name,phase] of Object.entries(resource.phases)) {
  if(!object(phase)||!object(phase.allocation)||!object(phase.workspace))fail('missing resource phase counters');
  if(integer(phase.samples,1)!==phaseSamples(name))fail('resource phase samples differ from protocol');
  const a=phase.allocation,w=phase.workspace;
  for(const key of ['calls','totalBytes','peakBytes','maxRetainedBytes'])integer(a[key]);
  if(a.totalBytes<a.peakBytes||a.peakBytes<a.maxRetainedBytes)fail('inconsistent allocation peak');
  if(a.totalBytes&&(!a.calls||!a.peakBytes))fail('allocation bytes lack calls or peak');
  for(const key of ['requests','maxPeakBytes','maxCurrentBytes','maxLimitBytes'])integer(w[key]);
  if(w.maxLimitBytes<w.maxPeakBytes||w.maxPeakBytes<w.maxCurrentBytes)fail('inconsistent reservation peak');
  if(w.requests&&(!w.maxLimitBytes||w.maxLimitBytes>16777216))fail('invalid observed workspace ceiling');
  if(!w.requests&&(w.maxPeakBytes||w.maxCurrentBytes||w.maxLimitBytes))fail('unobserved workspace has usage');
 }
 const probeOutcomes={};
 if(resource.schemaVersion===2){
  if(!sameKeys(resource.workspaceProbes,['traversal','checkpoint']))fail('missing workspace probes');
  for(const [name,probe] of Object.entries(resource.workspaceProbes)){
   if(!object(probe)||probe.api!=='execute_dsl'||!['observed','limit-exceeded'].includes(probe.status)||!object(probe.workspace))fail('unknown workspace probe outcome');
   const w=probe.workspace;
   for(const key of ['requests','maxPeakBytes','maxCurrentBytes','maxLimitBytes'])integer(w[key]);
   if(w.requests!==1||w.maxLimitBytes<=0||w.maxLimitBytes>16777216||w.maxLimitBytes<w.maxPeakBytes||w.maxPeakBytes<w.maxCurrentBytes)fail('invalid workspace probe reservation');
   probeOutcomes[name]=probe.status;
  }
 }
 const snapshots=resource.diskSnapshots;
 if(!Array.isArray(snapshots)||snapshots.length!==STAGES.length||snapshots.some((s,i)=>!object(s)||s.stage!==STAGES[i]))fail('missing or reordered disk lifecycle snapshots');
 for(const snapshot of snapshots) {
  if(!sameKeys(snapshot.categories,CATEGORIES))fail('missing disk categories');
  const categories=Object.values(snapshot.categories);
  if(categories.some(c=>!object(c)))fail('invalid disk category');
  for(const key of ['logicalBytes','files'])if(integer(snapshot[key])!==sum(categories.map(c=>c[key])))fail('disk totals differ from categories');
  if(snapshot.allocationSupport==='unavailable') {
   if(snapshot.allocatedBytes!==null||categories.some(c=>c.allocatedBytes!==null))fail('unavailable disk allocation must be null');
  } else if(snapshot.allocationSupport==='st_blocks_512') {
   if(integer(snapshot.allocatedBytes)!==sum(categories.map(c=>c.allocatedBytes)))fail('disk allocation totals differ');
  } else fail('unsupported disk allocation source');
  if(categories.some(c=>!c.files&&(c.logicalBytes||c.allocatedBytes)))fail('disk bytes lack regular files');
 }
 const phases=Object.values(resource.phases);
 return {maxTrackedPeakBytes:Math.max(...phases.map(p=>p.allocation.peakBytes)),
  maxReservedPeakBytes:phases.some(p=>p.workspace.requests)?Math.max(...phases.map(p=>p.workspace.maxPeakBytes)):null,
  maxLogicalBytes:Math.max(...snapshots.map(s=>s.logicalBytes)),
  maxAllocatedBytes:snapshots.some(s=>s.allocatedBytes===null)?null:Math.max(...snapshots.map(s=>s.allocatedBytes)),
  maxFiles:Math.max(...snapshots.map(s=>s.files)),
  ...(resource.schemaVersion===2?{workspaceProbeOutcomes:probeOutcomes}:{})};
}
function validateRawMetrics(run) {
 const phases=run.resources.schemaVersion===2?PHASES_V2:PHASES,metrics=[...phases,'sqliteTraversalUs','memoryTraversalUs'];
 if(!sameKeys(run.metrics,metrics)||!sameKeys(run.cppNewCalls,metrics))fail('missing raw resource metric inventory');
 for(const name of metrics) {
  const timings=run.metrics[name],calls=run.cppNewCalls[name],count=phaseSamples(name);
  if(!Array.isArray(timings)||!Array.isArray(calls)||timings.length!==count||calls.length!==count)fail('raw resource samples differ from protocol');
  if(timings.some(t=>typeof t!=='number'||!Number.isFinite(t)||t<0))fail('invalid resource timing sample');
  const total=sum(calls);
  if(phases.includes(name)&&total!==run.resources.phases[name].allocation.calls)fail('raw allocation calls differ from resource phase');
 }
}
function validateReport(report) {
 if(!object(report)||report.schemaVersion!==1||report.complete!==true||!Array.isArray(report.runFailures)||report.runFailures.length||typeof report.dirty!=='boolean'||!hex(report.revision,40)||!hex(report.sourceDigest,64))fail('incomplete or invalid resource provenance');
 const nodes={watch:250,phone:1000,desktop:10000}[report.profile],protocol=report.protocol;
 const version=protocol?.version??1;
 if(![1,2].includes(version))fail('unknown resource protocol version');
 const expected={mode:'resources',nodes,runs:1,samples:100,warmup:20,seed:42,...(version===2?{version:2,mutationBatchSize:250}:{})};
 if(!nodes||!sameKeys(protocol,Object.keys(expected))||Object.entries(expected).some(([k,v])=>protocol[k]!==v))fail('invalid resource protocol');
 if(!object(report.environment)||!['macos','ios-simulator'].includes(report.environment.platform))fail('unsupported resource environment');
 if(['hardware','os','architecture','toolchain'].some(k=>!text(report.environment[k])))fail('missing resource environment');
 if(report.environment.platform==='ios-simulator'&&(!object(report.environment.simulator)||['runtime','device'].some(k=>!text(report.environment.simulator[k]))))fail('missing simulator provenance');
 if(!Array.isArray(report.workloads)||report.workloads.length!==WORKLOADS.length||new Set(report.workloads.map(w=>w?.name)).size!==WORKLOADS.length||WORKLOADS.some(name=>!report.workloads.some(w=>w?.name===name)))fail('missing resource workloads');
 const summaries=[];
 for(const work of report.workloads) {
  if(!Array.isArray(work.runs)||work.runs.length!==1)fail('invalid resource run count');
  const run=work.runs[0];
  if(!object(run)||run.platform!==report.environment.platform||!object(run.dataset)||run.dataset.name!==work.name||run.dataset.nodes!==nodes||run.dataset.seed!==42)fail('foreign resource run provenance');
  const payload=work.name==='knowledge'?1024:work.name==='messages'?512:64;
  if(integer(run.dataset.edges,1)<1||run.dataset.nodeStringPayloadBytes!==payload||run.dataset.nodePropertyCount!==2||run.dataset.edgePropertyCount!==0)fail('resource dataset shape differs from workload');
  if(run.resources?.schemaVersion!==version)fail('resource schema differs from protocol');
  if(version===2){
   const shape={records:250,nodePropertyCount:1,scalarType:'int64',operation:'upsert-existing-node',durability:'wal-sync',database:'dedicated',samples:100,warmup:20,validation:'each-commit-and-reopen'};
   const outcome={singleCommits:120,batchCommits:120,mutations:30120,reopenedRecords:250,reopenVerified:true};
   const exact=(actual,expected)=>sameKeys(actual,Object.keys(expected))&&Object.entries(expected).every(([k,v])=>actual[k]===v);
   if(run.protocolVersion!==2||run.mutationBatchSize!==250||!exact(run.mutationProtocol,shape)||!exact(run.mutationValidation,outcome))fail('invalid mutation protocol or outcome');
  }else if((run.protocolVersion??1)!==1||Object.hasOwn(run,'mutationBatchSize')||Object.hasOwn(run,'mutationProtocol'))fail('mutation protocol cannot be historical evidence');
  summaries.push({name:work.name,...validateResources(run.resources)});
  validateRawMetrics(run);
 }
 if(report.evaluation!==undefined) {
  const e=report.evaluation;
  if(!object(e)||e.status!=='diagnostic-only'||e.hardwareQualified!==false||!Array.isArray(e.failures)||e.failures.length)fail('resource diagnostics cannot qualify timing or hardware');
  if(e.tables!==undefined&&(!Array.isArray(e.tables)||e.tables.length!==summaries.length||summaries.some(s=>{const row=e.tables.find(t=>t?.name===s.name);return !row||Object.entries(s).some(([k,v])=>k==='workspaceProbeOutcomes'?(!sameKeys(row[k],Object.keys(v))||Object.entries(v).some(([name,status])=>row[k][name]!==status)):row[k]!==v);})))fail('stale resource summary');
 }
 return summaries;
}

// Match Python json.dumps(files, sort_keys=True), including its ASCII escaping
// and default separators: this is the artifact producer's content digest format.
function artifactDigest(files) {
 const pythonString=s=>JSON.stringify(s).replace(/[\u007f-\uffff]/g,c=>'\\u'+c.charCodeAt(0).toString(16).padStart(4,'0'));
 const compare=(a,b)=>{const x=Array.from(a,c=>c.codePointAt(0)),y=Array.from(b,c=>c.codePointAt(0));for(let i=0;i<Math.min(x.length,y.length);i++)if(x[i]!==y[i])return x[i]-y[i];return x.length-y.length;};
 const json='{'+Object.keys(files).sort(compare).map(k=>pythonString(k)+': '+pythonString(files[k])).join(', ')+'}';
 return createHash('sha256').update(json).digest('hex');
}
function validateArtifact(report) {
 if(!object(report)||report.schemaVersion!==1||report.dirty!==false||!hex(report.revision,40)||!hex(report.artifactContentDigest,64)||!object(report.files)||!Object.keys(report.files).length)fail('invalid artifact provenance or missing inventory');
 const sizes=report.resourceSizes;
 if(!object(sizes)||sizes.schemaVersion!==1||sizes.dirty!==false||sizes.revision!==report.revision||sizes.artifactContentDigest!==report.artifactContentDigest||sizes.scope!==SIZE_SCOPE)fail('missing or foreign artifact size evidence');
 for(const [name,hash] of Object.entries(report.files))if(!name||name.startsWith('/')||name.split('/').some(p=>!p||p==='.'||p==='..')||name==='artifact-manifest.json'||!hex(hash,64))fail('unsafe artifact hash inventory');
 if(artifactDigest(report.files)!==report.artifactContentDigest)fail('stale artifact hash inventory');
 if(!sameKeys(sizes.files,Object.keys(report.files)))fail('missing artifact file sizes');
 for(const [name,hash] of Object.entries(report.files)) {
  const file=sizes.files[name];
  if(!object(file)||file.sha256!==hash)fail('artifact size hash differs from inventory');
  integer(file.bytes);
 }
 if(integer(sizes.fileCount,1)!==Object.keys(report.files).length||integer(sizes.bundleLogicalBytes,1)!==sum(Object.values(sizes.files).map(f=>f.bytes)))fail('artifact bundle size totals differ');
 if(!object(report.archive)||!text(report.archive.name)||!hex(report.archive.sha256,64)||report.archive.sha256!==sizes.archiveSha256||integer(report.archive.bytes,1)!==integer(sizes.archiveBytes,1))fail('artifact ZIP size provenance differs');
 if(!Array.isArray(report.slices)||report.slices.length!==SLICES.length||new Set(report.slices.map(s=>s?.id)).size!==SLICES.length||SLICES.some(id=>!report.slices.some(s=>s?.id===id))||!Array.isArray(sizes.slices)||sizes.slices.length!==SLICES.length)fail('missing artifact slice size matrix');
 for(const [i,slice] of report.slices.entries()) {
  integer(slice.installedBytes,1);integer(slice.compressedBytes,1);
  if(!hex(slice.sha256,64))fail('invalid artifact slice hash');
  const matches=Object.entries(report.files).filter(([,hash])=>hash===slice.sha256);
  if(!matches.length||matches.some(([name])=>sizes.files[name].bytes!==slice.installedBytes))fail('artifact slice size differs from files');
  if(!object(sizes.slices[i])||['id','installedBytes','compressedBytes','sha256'].some(k=>sizes.slices[i][k]!==slice[k]))fail('stale artifact slice size metadata');
 }
 return sizes;
}

/** Render indexed diagnostic reports; artifactReport is a separate verify --report JSON. */
export function renderResourceResults(reports,artifactReport=null) {
 if(!Array.isArray(reports))fail('missing resource report index');
 let md='\n## Resource diagnostics\n\n**Diagnostic only — instrumented, unqualified evidence.** These runs do not replace uninstrumented timing baselines or qualify hardware, production limits, or release readiness. Tracked C++ allocation bytes follow each report’s stated coverage; direct C malloc and Swift allocations are excluded. Instrumentation overhead and whole-process RSS are separate. Workspace reservations are estimates, not heap measurements. Tracked retained bytes describe phase boundaries; reserved current/peak/limit values summarize observed request contexts and are not a single simultaneous heap snapshot.\n\nDisk values are sampled lifecycle snapshots, not a transient peak or a long-running growth qualification. Allocated bytes use the reported filesystem support; unavailable values remain unavailable. No disk, file-count, or distribution-size budget is inferred.\n';
 if(!reports.length)md+='\nNo resource diagnostic reports are indexed; historical measurements remain unavailable.\n';
 const ids=new Set();
 for(const indexed of reports) {
  if(!object(indexed)||!object(indexed.entry)||!text(indexed.entry.id)||!text(indexed.entry.title)||ids.has(indexed.entry.id))fail('missing or duplicate resource index entry');
  ids.add(indexed.entry.id);
  const r=indexed.report,summaries=validateReport(r);
  md+=`\n### ${escape(indexed.entry.title)}\n\nReport ${escape(indexed.entry.id)}; ${escape(r.environment.platform)} / ${escape(r.profile)}. Schema 1, instrumented resource protocol version ${r.protocol.version??1}: 1 independent process per workload, ${r.protocol.nodes} nodes, 100 point reads, 20 warmups, seed 42. Revision \`${r.revision}\`${r.dirty?' plus working tree changes':''}; source digest \`${r.sourceDigest}\`.\n`;
  md+=`\nEnvironment: ${['hardware','os','architecture','toolchain'].map(k=>escape(r.environment[k])).join('; ')}.${r.environment.simulator?' Simulator '+escape(r.environment.simulator.device)+' / '+escape(r.environment.simulator.runtime)+'.':''}\n`;
  md+=table(['Workload','Max tracked peak bytes','Max reserved peak bytes','Max sampled logical bytes','Max sampled allocated bytes','Max sampled files'],summaries.map(s=>[s.name,s.maxTrackedPeakBytes,shown(s.maxReservedPeakBytes),s.maxLogicalBytes,shown(s.maxAllocatedBytes),s.maxFiles]));
  for(const work of r.workloads) {
   const resource=work.runs[0].resources;
   md+=`\n#### ${escape(work.name)}\n\nCoverage: ${escape(resource.coverage.replace(/\.+$/, ''))}. Values are exact byte counts. A workspace phase with zero observed requests is shown as Unavailable, not zero usage.\n`;
   md+=table(['Phase','Samples','Tracked peak bytes','Tracked retained bytes','Tracked total bytes','Allocation calls','Observed requests','Reserved peak bytes','Reserved current bytes','Reservation limit bytes'],(resource.schemaVersion===2?PHASES_V2:PHASES).map(name=>{const p=resource.phases[name],a=p.allocation,w=p.workspace;return [name.slice(0,-2),p.samples,a.peakBytes,a.maxRetainedBytes,a.totalBytes,a.calls,w.requests,...['maxPeakBytes','maxCurrentBytes','maxLimitBytes'].map(k=>w.requests?w[k]:'Unavailable')];}));
   if(resource.schemaVersion===2){
    md+='\nSeparate workspace probes use the DSL API after the sampled lifecycle. They include parser and serialization reservations and do not observe the direct traversal/checkpoint/close phases. A limit-exceeded outcome records admission failure, not complete operation workspace. These probes are excluded from the phase maximum above.\n';
    md+=table(['Probe','API','Outcome','Observed requests','Reserved peak bytes','Reserved current bytes','Reservation limit bytes'],Object.entries(resource.workspaceProbes).map(([name,p])=>[name,p.api,p.status,p.workspace.requests,p.workspace.maxPeakBytes,p.workspace.maxCurrentBytes,p.workspace.maxLimitBytes]));
   }
   const rows=[];
   for(const snapshot of resource.diskSnapshots)for(const [category,counters] of [['total',snapshot],...Object.entries(snapshot.categories)])rows.push([snapshot.stage,category,counters.logicalBytes,shown(counters.allocatedBytes),counters.files,snapshot.allocationSupport]);
   md+=table(['Lifecycle snapshot','Category','Logical bytes','Allocated bytes','Files','Allocation source'],rows);
  }
 }
 if(artifactReport===null||artifactReport===undefined)return md+'\nArtifact size refresh unavailable; historical native archive sizes are not current distribution measurements.\n';
 const sizes=validateArtifact(artifactReport);
 md+=`\n### Fresh artifact sizes\n\nSize schema 1; clean revision \`${sizes.revision}\`; artifact content digest \`${sizes.artifactContentDigest}\`. ${escape(sizes.scope)} These verified distribution measurements are separate from runtime qualification; no consumer execution or physical-device support is established by size verification.\n`;
 md+=table(['Measurement','Bytes','Regular files'],[['Extracted bundle excluding artifact-manifest.json',sizes.bundleLogicalBytes,sizes.fileCount],['Complete verified ZIP including manifest',sizes.archiveBytes,'Not applicable']]);
 md+=`\nArchive ${escape(artifactReport.archive.name)}; SHA-256 \`${sizes.archiveSha256}\`.\n`;
 md+=table(['Slice','Native archive bytes','Independent ZIP bytes','Native archive SHA-256'],sizes.slices.map(s=>[s.id,s.installedBytes,s.compressedBytes,s.sha256]));
 return md;
}
