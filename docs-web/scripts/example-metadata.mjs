import {readFile, readdir, realpath} from 'node:fs/promises';
import {resolve, relative, isAbsolute, sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const fail = message => {throw new Error(message);};
export async function exampleMetadata(root) {
 root = await realpath(root);
 const inputs = new Map();
 async function source(path) {
  if(typeof path !== 'string' || !path || isAbsolute(path)) fail('invalid example source path');
  const physical = await realpath(resolve(root,path)), rel = relative(root,physical);
  if(rel==='..' || rel.startsWith(`..${sep}`)) fail(`example source outside repository: ${path}`);
  const body = await readFile(physical,'utf8'); inputs.set(path,body); return body;
 }
 const manifest = JSON.parse(await source('examples/manifest.json'));
 if(manifest.schemaVersion !== 1 || !Array.isArray(manifest.examples)) fail('invalid example manifest');
 const ids = new Set();
 for(const e of manifest.examples) {
  if(!/^[a-z0-9-]+$/.test(e.id) || ids.has(e.id)) fail(`duplicate or invalid example ID: ${e.id}`); ids.add(e.id);
  if(!['swift','ngql','cpp'].includes(e.language) || !['run','compile','fragment'].includes(e.mode)) fail(`invalid example language/mode: ${e.id}`);
  if(!e.setupGroup || !Array.isArray(e.setup) || !Array.isArray(e.platforms) || !e.platforms.length) fail(`missing example setup/platforms: ${e.id}`);
  await source(e.path);
  if(e.mode==='fragment') {if(!e.limitations)fail(`fragment needs limitations: ${e.id}`); continue;}
  if(e.language==='ngql') {
   if(e.mode!=='run')fail(`NGQL must run: ${e.id}`);
   for(const path of e.setup) await source(path);
   JSON.parse(await source(e.expected));
  } else if(e.language==='cpp') {
   if(e.mode!=='run' || e.setup.length || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(e.entryPoint||''))fail(`C++ examples require a self-contained run entryPoint: ${e.id}`);
  } else if(e.language==='swift') {
   if(typeof e.expectedStdout!=='string' || !e.expectedStdout)fail(`Swift needs output assertion: ${e.id}`);
   if(e.assertions) await source(e.assertions);
  }
 }
 for(const e of manifest.examples.filter(e=>e.language==='swift' && e.mode!=='fragment')) {
  for(const id of e.setup) {
   const dependency = manifest.examples.find(other=>other.id===id);
   if(!dependency || dependency.setupGroup!==e.setupGroup || manifest.examples.indexOf(dependency)>=manifest.examples.indexOf(e))fail(`invalid setup dependency: ${e.id}/${id}`);
  }
  const group = manifest.examples.filter(other=>other.setupGroup===e.setupGroup);
  if(!group.some(other=>other.mode==='run') || group.some(other=>other.language!==e.language || other.expectedStdout!==e.expectedStdout || other.assertions!==e.assertions))fail(`inconsistent setup group: ${e.setupGroup}`);
 }
 function digest(map) {const hash=createHash('sha256');for(const [path,body] of [...map].sort(([a],[b])=>a<b?-1:a>b?1:0))hash.update(path+'\0'+body+'\0');return hash.digest('hex');}
 const inputDigest = digest(inputs); const engine = new Map();
 for(const folder of ['cpp/src','cpp/include','cpp/c_api','cpp/third_party','swift/Sources']) {
  let entries;try{entries=await readdir(resolve(root,folder),{recursive:true,withFileTypes:true});}catch(error){if(error.code==='ENOENT')continue;throw error;}
  for(const entry of entries) if(entry.isFile()) {
   const path = relative(root,resolve(entry.parentPath,entry.name)).split(sep).join('/');
   engine.set(path,await source(path));
  }
 }
 for(const path of ['cpp/CMakeLists.txt','Package.swift','tools/doc-example-runner.cpp','tools/test-doc-examples.py']) {
  try{engine.set(path,await source(path));}catch(error){if(error.code!=='ENOENT')throw error;}
 }
 return {engineDigest:digest(engine),inputDigest};
}
if(process.argv[1] && resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
 try{console.log(JSON.stringify(await exampleMetadata(process.argv[2])));}catch(error){console.error(error.message);process.exitCode=1;}
}
