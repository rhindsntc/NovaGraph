import React, { useEffect, useRef, useState } from 'react';
import { Database, Play, RotateCcw } from 'lucide-react';
import { GraphCanvas } from './GraphCanvas';
import { PLAYGROUND_PRESETS, PlaygroundPreset } from '../data/playgroundData';
import type { VisualNode } from '../types/docs';
import { localRunnerConfig, parseDisplayJSON, requestRunner, resultGraph, RunnerError, RunnerUnavailable } from '../lib/runner';

type Inspection = {kind: string; raw: string; nextCursor: string | null};
export const Playground: React.FC = () => {
 const [config] = useState(localRunnerConfig);
 const [preset,setPreset] = useState(PLAYGROUND_PRESETS[0]);
 const [query,setQuery] = useState(preset.query);
 const [live,setLive] = useState(false), [connected,setConnected] = useState(false), [busy,setBusy] = useState(false);
 const [output,setOutput] = useState(''), [error,setError] = useState('');
 const [inspection,setInspection] = useState<Inspection|null>(null);
 const [selected,setSelected] = useState<VisualNode|null>(null), [tab,setTab] = useState<'json'|'visual'>('json');
 const [databasePath,setDatabasePath] = useState('');
 const pending = useRef<AbortController|null>(null);
 useEffect(()=>()=>{pending.current?.abort();pending.current=null;},[]);
 const recorded = preset.recorded.outputText;
 const display = live ? output : recorded;
 const graph = resultGraph(display ? parseDisplayJSON(display) : null);
 const clear = () => {setOutput('');setInspection(null);setSelected(null);setError('');};
 async function invoke(path: string, payload: unknown, action: 'connect'|'query'|'inspect'|'reset', kind='') {
  if(!config || busy) return;
  const controller=new AbortController();pending.current=controller;
  setBusy(true);setError('');setSelected(null);
  if(action==='inspect') setInspection(null);else clear();
  const timeout=window.setTimeout(()=>controller.abort(),7000);
  try {
   const result=await requestRunner(config,path,payload,controller.signal);
   if(pending.current!==controller) return;
   if(action==='connect') {
    const data=result.envelope.data as {mode:string;databasePath:string};
    if(data.mode!==config.mode || typeof data.databasePath!=='string') throw new RunnerUnavailable('Runner unavailable: session changed. Reload this page.');
    setConnected(true);setDatabasePath(data.databasePath);
   } else if(action==='inspect') {
    const data=result.envelope.data as {kind:string;items:unknown[];nextCursor:string|null};
    if(data.kind!==kind || !Array.isArray(data.items) || (data.nextCursor!==null && typeof data.nextCursor!=='string')) throw new RunnerUnavailable('Runner unavailable: invalid inspection page.');
    setInspection({kind,raw:result.raw,nextCursor:data.nextCursor});
   } else {
    setOutput(result.raw);
    if(action==='reset') setDatabasePath((result.envelope.data as {databasePath:string}).databasePath);
   }
  } catch(cause) {
   if(pending.current!==controller) return;
   clear();setTab('json');setError(cause instanceof Error ? cause.message : String(cause));
   if(cause instanceof RunnerError) {
    setOutput(cause.raw);
    if(cause.envelope.error?.code==='forbidden') setConnected(false);
   } else setConnected(false);
  } finally {
   window.clearTimeout(timeout);
   if(pending.current===controller) {pending.current=null;setBusy(false);}
  }
 }
 const selectPreset = (next: PlaygroundPreset) => {setPreset(next);setQuery(next.query);clear();};
 const inspect = (kind: string, cursor?: string|null) => invoke(`/dev/inspect/${kind}`,{limit:50,cursor},'inspect',kind);
 return <div className="doc-content fade-in playground-shell">
  <div className="doc-header"><h1 className="doc-title">NGQL Playground</h1>
   <p className="doc-lead">{live ? 'Run NGQL against your local Nova engine. Writes change the selected database.' : 'Recorded examples from the actual engine. The query and output below are read-only.'}</p>
   <p role="status">{live ? connected ? `Live · ${config?.mode}` : 'Live · disconnected' : 'Recorded example · read-only'}</p>
   {!live && <p>Fixture revision <code>{preset.recorded.revision}</code>. <a href="#developer-tools">Local runner setup and inspection guide</a></p>}
   {config && !connected && <button className="run-query-btn" disabled={busy} onClick={()=>{setLive(true);void invoke('/dev/session',{},'connect');}}>Connect local runner</button>}
   {live && databasePath && <p className="runner-path">Database: <code>{databasePath}</code></p>}
   {live && config?.mode==='persistent' && <p>Explicitly persistent session. Writes survive restart; browser reset is disabled.</p>}
  </div>
  <div className="playground-controls"><div className="preset-selector">
   {PLAYGROUND_PRESETS.map(p=><button key={p.id} className={`preset-btn ${preset.id===p.id?'active':''}`} disabled={busy} onClick={()=>selectPreset(p)}>{p.name}</button>)}
  </div>{connected && <button className="run-query-btn" disabled={busy} onClick={()=>void invoke('/dev/query',{query},'query')}><Play size={15}/>Run query</button>}</div>
  {error && <p role="alert" className="runner-error">{error}</p>}
  <div className="playground-grid">
   <div className="editor-card"><div className="panel-header"><span><Database size={16}/> {connected?'NGQL Query Editor':'Recorded NGQL'}</span>
    {connected && <button className="icon-btn" aria-label="Restore preset query" disabled={busy} onClick={()=>{setQuery(preset.query);clear();}}><RotateCcw size={14}/></button>}
   </div><textarea aria-label="NGQL query" className="ngql-editor-textarea" readOnly={!connected} disabled={busy} value={query} onChange={event=>{setQuery(event.target.value);clear();}} spellCheck={false}/></div>
   <div className="visualizer-card"><div className="panel-header">
    <button className={`code-tab-btn ${tab==='json'?'active':''}`} aria-pressed={tab==='json'} onClick={()=>setTab('json')}>{live?'Engine JSON':'Recorded JSON'}</button>
    <button className={`code-tab-btn ${tab==='visual'?'active':''}`} aria-pressed={tab==='visual'} onClick={()=>setTab('visual')}>Result graph</button>
   </div>
   {!live && <p className="runner-caption">Recorded real-engine output for {preset.name}</p>}
   <pre data-testid="query-output" className="code-content runner-output" hidden={tab!=='json'}>{display}</pre>
   {tab==='visual' && <><GraphCanvas graph={graph} selectedNodeId={selected?.id} onSelectNode={setSelected}/>
    <p className="runner-caption">Returned records only, up to 200 nodes and 400 edges. An edge is drawn when both endpoints are present. Projections, mutation receipts, plans and complete paths are in JSON.</p>
    {selected && <pre className="code-content">{JSON.stringify(selected,null,2)}</pre>}</>}
   </div>
  </div>
  {connected && <section aria-label="Database inspector" className="runner-inspector">
   <h2>Inspect the database</h2><p>Pages contain up to 50 actual records or declared property indexes. A write invalidates existing page cursors; start inspection again.</p>
   <div className="playground-controls">{['nodes','edges','indexes'].map(kind=><button className="preset-btn" key={kind} disabled={busy} onClick={()=>void inspect(kind)}>Inspect {kind}</button>)}</div>
   {inspection && <><pre data-testid="inspection-output" className="code-content runner-output">{inspection.raw}</pre>
    {inspection.nextCursor && <button className="preset-btn" disabled={busy} onClick={()=>void inspect(inspection.kind,inspection.nextCursor)}>Next page</button>}</>}
   {config?.mode==='disposable' && <><p>Reset discards this session’s disposable data. Preset selection only changes the editor; Run query performs its writes.</p>
    <button className="preset-btn" disabled={busy} onClick={()=>void invoke('/dev/reset',{},'reset')}>Reset disposable database</button></>}
  </section>}
 </div>;
};
