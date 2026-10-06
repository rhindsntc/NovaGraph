import type { VisualGraph, VisualNode, VisualEdge } from '../types/docs';

export interface Envelope { schemaVersion: 2; ok: boolean; data?: unknown; error?: {code: string; message: string; context?: unknown}; receipt?: unknown }
export interface RunnerConfig { token: string; mode: 'disposable' | 'persistent' }
export function localRunnerConfig(): RunnerConfig | null {
 const token = document.querySelector<HTMLMetaElement>('meta[name="nova-session"]')?.content;
 const mode = document.querySelector<HTMLMetaElement>('meta[name="nova-mode"]')?.content;
 if(location.protocol !== 'http:' || location.hostname !== '127.0.0.1' || !token || !/^[A-Fa-f0-9-]{72}$/.test(token) || (mode !== 'disposable' && mode !== 'persistent')) return null;
 return {token,mode};
}
// Display only: retain integer lexemes outside JS's safe range as strings. The raw
// response remains untouched in the JSON panel. Quoted strings are never rewritten.
export function parseDisplayJSON(text: string): unknown {
 return JSON.parse(text.replace(/"(?:[^"\\]|\\.)*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/g, token =>
  /^-?\d+$/.test(token) && !Number.isSafeInteger(Number(token)) ? JSON.stringify(token) : token));
}
export class RunnerUnavailable extends Error {}
export class RunnerError extends Error {
 constructor(public envelope: Envelope, public raw: string) { super(`${envelope.error?.code}: ${envelope.error?.message}`); }
}
export async function requestRunner(config: RunnerConfig, path: string, payload: unknown, signal: AbortSignal): Promise<{envelope: Envelope; raw: string}> {
 let response: Response, raw: string;
 try {
  response = await fetch(path,{method:'POST',mode:'same-origin',credentials:'omit',redirect:'error',
   headers:{'Content-Type':'application/json','X-Nova-Session':config.token},body:JSON.stringify(payload),signal});
  raw = await response.text();
 } catch { throw new RunnerUnavailable('Runner unavailable. Execution outcome may be unknown; inspect before retrying a write. Restart or reload the local runner to reconnect.'); }
 let envelope: Envelope;
 try { envelope = parseDisplayJSON(raw) as Envelope; } catch { throw new RunnerUnavailable('Runner unavailable: invalid JSON response.'); }
 if(!envelope || envelope.schemaVersion!==2 || typeof envelope.ok!=='boolean' || (!envelope.ok && (!envelope.error || typeof envelope.error.code!=='string' || typeof envelope.error.message!=='string')) || (envelope.ok && !Object.hasOwn(envelope,'data'))) throw new RunnerUnavailable('Runner unavailable: unsupported response schema.');
 if(!response.ok || !envelope.ok) throw new RunnerError(envelope,raw);
 return {envelope,raw};
}
export function resultGraph(value: unknown): VisualGraph {
 const nodes = new Map<string,VisualNode>(), edges = new Map<string,VisualEdge>();
 const props = (value: unknown): Record<string,unknown> => value && typeof value==='object' && !Array.isArray(value) ?
  Object.fromEntries(Object.entries(value).map(([key,tag])=>[key,tag && typeof tag==='object' && 'type' in tag && 'value' in tag ? tag.value : tag])) : {};
 function visit(value: unknown, depth=0) {
  if(depth>16 || !value || typeof value!=='object') return;
  if(Array.isArray(value)) { value.forEach(item=>visit(item,depth+1)); return; }
  const object=value as Record<string,unknown>;
  // Only full native records count; projection objects are shown as JSON.
  if(object.kind==='node' && typeof object.id==='string' && typeof object.label==='string' && object.properties && nodes.size<200) {
   nodes.set(object.id,{id:object.id,label:object.label,properties:props(object.properties)});
  } else if(object.kind==='edge' && typeof object.from==='string' && typeof object.to==='string' && typeof object.type==='string' && edges.size<400) {
   edges.set(JSON.stringify([object.from,object.type,object.to]),{from:object.from,type:object.type,to:object.to,properties:props(object.properties)});
  } else {
   // Traverse known engine containers only; user properties cannot impersonate records.
   for(const key of ['data','results','nodes','edges','paths','items']) if(Object.hasOwn(object,key)) visit(object[key],depth+1);
  }
 }
 visit(value);return {nodes:[...nodes.values()],edges:[...edges.values()]};
}
