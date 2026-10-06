import {readFile, writeFile, mkdir, realpath} from 'node:fs/promises';
import {resolve, relative, dirname, isAbsolute, sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {marked, Marked} from 'marked';
import {exampleMetadata} from './example-metadata.mjs';

const OUTPUT='docs-web/src/data/generated/docs.json';
const fail=message=>{throw new Error(message);};
const text=(value,label)=>typeof value==='string' && value.trim() ? value : fail(`${label} must be a nonempty string`);
const array=(value,label)=>Array.isArray(value) ? value : fail(`${label} must be an array`);
function unique(items,label) {
 const result=new Map();for(const item of array(items,label)) {
  if(!item || typeof item!=='object') fail(`${label} entry must be an object`);
  const id=text(item.id,`${label} id`);if(!/^[A-Za-z0-9_.-]+$/.test(id))fail(`invalid ${label} id: ${id}`);
  if(result.has(id))fail(`duplicate ${label} id: ${id}`);result.set(id,item);
 }return result;
}
const clean=s=>s.replace(/[`*_>#]/g,'').replace(/\s+/g,' ').trim();
function slug(text) {return text.toLowerCase().replace(/<[^>]*>/g,'').replace(/[^\p{L}\p{N}_\s-]/gu,'').trim().replace(/\s/g,'-');}
function anchors(markdown) {
 const result=new Set();const counts=new Map();
 marked.walkTokens(marked.lexer(markdown),token=>{
  if(token.type==='heading') {
   const base=slug(token.text);
   const count=counts.get(base)||0;counts.set(base,count+1);result.add(count ? `${base}-${count}` : base);
  }
 });return result;
}
export async function buildDocs(root,options={}) {
 root=await realpath(root);const inputs=new Map();
 function publicSource(path,name) {
  const rel=relative(root,path).split(sep).join('/');
  if(rel==='..'||rel.startsWith('../')||isAbsolute(rel))fail(`source outside repository: ${name}`);
  if(rel==='docs'||rel.startsWith('docs/'))fail(`private docs source is not publishable: ${name}`);
  if(!rel.startsWith('docs-web/content/')&&!rel.startsWith('examples/')&&rel!=='tests/catalog.json')fail(`not an allowed public source: ${name}`);
  return rel;
 }
 async function source(name) {
  text(name,'source path');if(isAbsolute(name))fail(`unsafe absolute source: ${name}`);
  const path=resolve(root,name);const rel=publicSource(path,name);
  const physical=await realpath(path);publicSource(physical,name);
  const content=await readFile(physical,'utf8');inputs.set(rel,content);return content;
 }
 const json=async name=>JSON.parse(await source(name));
 const site=await json('docs-web/content/site.json');const featureDoc=await json('docs-web/content/features.json');const catalog=await json('tests/catalog.json');
 for(const [name,value] of [['site',site],['features',featureDoc],['test catalog',catalog]])if(value.schemaVersion!==1)fail(`unsupported ${name} schemaVersion`);
 text(site.version,'version');if(!/^[0-9a-f]{40}$/.test(site.reviewedRevision))fail('reviewedRevision must be a full commit hash');
 for(const flag of ['performanceMetadata','artifactMetadata'])if(Object.hasOwn(site,flag))fail(`deprecated public site metadata: ${flag}`);
 const categories=unique(site.categories,'category'),articles=unique(site.articles,'article'),snippets=unique(site.snippets,'snippet');
 const tests=unique(catalog.tests,'test'),features=unique(featureDoc.features,'feature'),presets=unique(site.presets,'preset');
 for(const test of tests.values()) {
  if(!['cpp','swift','docs','devtools','package','artifact','sample','performance'].includes(test.language) || !test.id.startsWith(test.language+'.'))fail(`invalid test language: ${test.id}`);
  text(test.suite,'test suite');
  if(typeof test.owner!=='string' || (test.owner && !/^T\d{2}$/.test(test.owner)))fail(`invalid test owner: ${test.id}`);
 }
 for(const category of categories.values())text(category.title,'category title');
 const examples=unique((await json('examples/manifest.json')).examples,'example');
 const exampleTestIds=[...examples.values()].filter(e=>e.mode!=='fragment').map(e=>'docs.'+e.id).sort();
 if(JSON.stringify([...tests.values()].filter(t=>t.language==='docs').map(t=>t.id).sort())!==JSON.stringify(exampleTestIds))fail('documentation test inventory differs from executable manifest');
 const recorded=await json('examples/recorded-results.json');
 // Check every manifest file through the public boundary before the provenance
 // helper reads it. Engine files remain separate provenance-only inputs.
 for(const example of examples.values()) {
  await source(example.path);
  if(example.mode==='fragment')continue;
  if(example.language==='ngql') {
   for(const path of array(example.setup,'example setup'))await source(path);
   await source(example.expected);
  } else if(example.language==='swift'&&example.assertions)await source(example.assertions);
 }
 const metadata=await exampleMetadata(root);
 if(recorded.schemaVersion!==1 || !/^[0-9a-f]{40}$/.test(recorded.revision))fail('invalid recorded example revision');
 if(recorded.engineDigest!==metadata.engineDigest || recorded.inputDigest!==metadata.inputDigest)fail('recorded examples are stale; run docs:examples --record after reviewing expectations');
 const ngqlIds=[...examples.values()].filter(e=>e.mode==='run' && e.language==='ngql').map(e=>e.id).sort();
 if(JSON.stringify(Object.keys(recorded.outputs).sort())!==JSON.stringify(ngqlIds))fail('recorded example set differs from executable manifest');
 for(const id of ngqlIds)if(JSON.stringify(recorded.outputs[id])!==JSON.stringify(await json(examples.get(id).expected)))fail(`recorded output differs from expected: ${id}`);
 const snippetData=new Map();
 for(const snippet of snippets.values()) {
  const example=examples.get(snippet.id);
  if(!example || example.path!==snippet.path || example.language!==snippet.language)fail(`snippet source differs from executable manifest: ${snippet.id}`);
  snippetData.set(snippet.id,{label:text(snippet.label,'snippet label'),language:text(snippet.language,'snippet language'),code:await source(snippet.path),
   example:{id:example.id,mode:example.mode,setupGroup:example.setupGroup,setup:example.setup,platforms:example.platforms,limitations:example.limitations||'',revision:recorded.revision,output:recorded.outputs[example.id]||null}});
 }
 for(const feature of features.values()) {
  if(!/^(SCOPE|SWIFT|STORE|INDEX|NGQL|TXN|PKG|PERF|TEST|DX)-\d{3}$/.test(feature.id))fail(`invalid feature id: ${feature.id}`);
  if(!['planned','partial','implemented','verified'].includes(feature.status))fail(`invalid status: ${feature.id}`);
  text(feature.title,'feature title');text(feature.acceptance,'feature acceptance');
  if(feature.introducedVersion!==null)text(feature.introducedVersion,'introducedVersion');
  for(const id of array(feature.testIds,'feature testIds'))if(!tests.has(id))fail(`unknown test ${id} in ${feature.id}`);
  for(const id of array(feature.articleIds,'feature articleIds'))if(!articles.has(id))fail(`unknown article ${id} in ${feature.id}`);
  for(const owner of array(feature.ownerTasks,'ownerTasks'))if(!/^T\d{2}$/.test(owner))fail(`invalid owner task: ${owner}`);
 }
 const orderedFeatures=[...features.values()];
 async function validateLinks(content,name) {
  const links=[];marked.walkTokens(marked.lexer(content),token=>{if(token.type==='link'||token.type==='image')links.push({href:token.href,type:token.type});});
  for(const {href,type} of links) {
   if(/^(https?:|mailto:)/i.test(href))continue;
   if(/^[a-z][a-z0-9+.-]*:/i.test(href)||href.startsWith('//'))fail(`unsafe link ${href} in ${name}`);
   const [file,fragment]=href.split('#');
   const target=file?relative(root,resolve(root,dirname(name),decodeURIComponent(file))):name;
   if(type==='image') {
    if(!target.startsWith('docs-web/public/images/') || !/\.(png|jpe?g|webp)$/i.test(target) || fragment)fail(`unpublished local destination ${href} in ${name}`);
    const physical=await realpath(resolve(root,target));
    const imageRelative=relative(resolve(root,'docs-web/public/images'),physical);
    if(imageRelative==='..'||imageRelative.startsWith(`..${sep}`)||isAbsolute(imageRelative))fail(`unpublished local destination ${href} in ${name}`);
    continue;
   }
   let body;try{body=await source(target);}catch(error){fail(`broken link ${href} in ${name}: ${error.message}`);}
   if(fragment && !anchors(body).has(decodeURIComponent(fragment)))fail(`broken anchor ${href} in ${name}`);
  }
 }
 // First index the actual headings published by every article. Repository files
 // alone are not web destinations, and section articles retain source slugs.
 const drafts=[];
 for(const article of articles.values()) {
  if(!categories.has(article.categoryId))fail(`unknown category ${article.categoryId}`);
  for(const id of array(article.relatedIds||[],'relatedIds'))if(!articles.has(id))fail(`unknown related article: ${id}`);
  const ids=array(article.snippetIds,'article snippetIds');
  const codeTabs=ids.map(id=>snippetData.get(id)||fail(`unknown snippet ${id}`));
  let content=await source(text(article.source,'article source'));
  content=content.replace(/<!--\s*snippet:\s*([\w.-]+)\s*-->/g,(_,id)=>{
   const snippet=snippetData.get(id)||fail(`unknown snippet ${id}`);return `\n\`\`\`${snippet.language}\n${snippet.code.trimEnd()}\n\`\`\`\n`;
  });
  let tokens=marked.lexer(content);const counts=new Map();
  marked.walkTokens(tokens,token=>{
   if(token.type==='heading') {
    const base=slug(token.text),count=counts.get(base)||0;counts.set(base,count+1);
    token.docAnchor=count?`${base}-${count}`:base;
   }
  });
  if(article.anchor) {
   const start=tokens.findIndex(token=>token.type==='heading' && token.docAnchor===article.anchor);
   if(start<0)fail(`missing source anchor ${article.anchor}`);
   let end=start+1;
   while(end<tokens.length && !(tokens[end].type==='heading' && tokens[end].depth<=tokens[start].depth))end++;
   tokens=Object.assign(tokens.slice(start,end),{links:tokens.links});
   content=tokens.map(token=>token.raw).join('');
  }
  await validateLinks(content,article.source);
  const heading=tokens.find(t=>t.type==='heading'),paragraph=tokens.find(t=>t.type==='paragraph');
  if(!heading)fail(`article ${article.id} needs a Markdown heading`);
  const headings=new Set();marked.walkTokens(tokens,token=>{if(token.type==='heading')headings.add(token.docAnchor);});
  drafts.push({article,tokens,headings,content,codeTabs,title:clean(heading.text),lead:paragraph?clean(paragraph.text):clean(heading.text),
   source:relative(root,resolve(root,article.source))});
 }
 const compiled=[];
 for(const draft of drafts) {
  const {article,tokens,content,codeTabs,title,lead}=draft;
  marked.walkTokens(tokens,token=>{
   if(!['link','image'].includes(token.type) || /^(https?:|mailto:)/i.test(token.href))return;
   const [file,encodedFragment]=token.href.split('#');
   const fragment=encodedFragment?decodeURIComponent(encodedFragment):'';
   const target=file?relative(root,resolve(root,dirname(draft.source),decodeURIComponent(file))):draft.source;
   if(token.type==='image') {token.href='./'+target.slice('docs-web/public/'.length).split('/').map(encodeURIComponent).join('/');return;}
   const candidates=drafts.filter(d=>d.source===target && (fragment?d.headings.has(fragment):!d.article.anchor));
   const destination=candidates.find(d=>d===draft) || candidates.find(d=>d.article.anchor===fragment) || candidates[0];
   if(!destination || token.type==='image')fail(`unpublished local destination ${token.href} in ${article.source}`);
   token.href=`#${destination.article.id}${fragment?'/'+encodeURIComponent(fragment):''}`;
  });
  const parser=new Marked({gfm:true,renderer:{heading(token){
   return `<h${token.depth} id="${article.id}/${token.docAnchor}">${this.parser.parseInline(token.tokens)}</h${token.depth}>\n`;
  }}});
  const html=parser.parser(tokens,parser.defaults);
  compiled.push({id:article.id,relatedIds:article.relatedIds||[],title,categoryId:article.categoryId,html,category:categories.get(article.categoryId).title,lead,content,codeTabs,source:article.source});
 }
 const compiledPresets=[];
 for(const preset of presets.values()) {
  const example=examples.get(preset.exampleId);
  if(!example || example.mode!=='run' || example.language!=='ngql' || example.path!==preset.querySource)fail(`preset requires a runnable NGQL example: ${preset.id}`);
  const query=await source(text(preset.querySource,'preset querySource'));
  compiledPresets.push({id:preset.id,name:text(preset.name,'preset name'),description:text(preset.description,'preset description'),query,recorded:{revision:recorded.revision,outputText:await source(example.expected)}});
 }
 const digest=createHash('sha256');for(const [name,body] of [...inputs].sort(([a],[b])=>a.localeCompare(b)))digest.update(name+'\0'+body+'\0');
 const sourceDigest=digest.digest('hex');
 for(const feature of features.values())if(feature.status==='verified') {
  if(feature.testIds.some(id=>tests.get(id).owner))fail(`verified feature ${feature.id} still references known release blockers`);
  const evidence=options.evidence;
  if(!evidence || !options.revision || evidence.revision!==options.revision || evidence.sourceDigest!==sourceDigest)fail(`verified feature ${feature.id} requires revision-matched evidence`);
  if(!feature.testIds.length || feature.testIds.some(id=>!evidence.tests?.some(t=>t.id===id && t.outcome==='passed')))fail(`missing passing evidence for ${feature.id}`);
 }
 const search=[];
 for(const draft of drafts) {
  const article=compiled.find(a=>a.id===draft.article.id);
  const keywords=[article.id,...[...features.values()].filter(f=>f.articleIds.includes(article.id)).map(f=>f.id)];
  search.push({id:`s-${article.id}`,articleId:article.id,title:article.title,category:article.category,preview:article.lead,content:clean(article.content),keywords});
  for(let i=1;i<draft.tokens.length;i++) {
   const heading=draft.tokens[i];if(heading.type!=='heading')continue;
   let end=i+1;while(end<draft.tokens.length && draft.tokens[end].type!=='heading')end++;
   const content=clean(draft.tokens.slice(i+1,end).map(t=>t.raw).join(''));
   search.push({id:`s-${article.id}-${heading.docAnchor}`,articleId:`${article.id}/${encodeURIComponent(heading.docAnchor)}`,title:clean(heading.text),category:article.category,preview:content.slice(0,180),content,keywords});
  }
 }
 const platforms=array(site.platforms||[],'platforms');for(const platform of platforms){text(platform.name,'platform name');text(platform.status,'platform status');if(platform.minimum!==null)text(platform.minimum,'platform minimum');}
 const limitations=array(site.limitations||[],'limitations');for(const limit of limitations)text(limit,'limitation');
 const data={schemaVersion:1,version:site.version,reviewedRevision:site.reviewedRevision,sourceDigest,platforms,limitations,examplesRevision:recorded.revision,
  categories:[...categories.values()].map(c=>({id:c.id,title:c.title,articles:compiled.filter(a=>a.categoryId===c.id)})),
  search,
  features:orderedFeatures,presets:compiledPresets};
 return {outputs:{[OUTPUT]:JSON.stringify(data,null,2)+'\n'}};
}
export async function generateDocs(root,options={}) {
 const result=await buildDocs(root,options);const stale=[];
 for(const [name,content] of Object.entries(result.outputs)) {
  const path=resolve(root,name);
  if(options.check) {let current;try{current=await readFile(path,'utf8');}catch{}if(current!==content)stale.push(name);}
  else {await mkdir(dirname(path),{recursive:true});await writeFile(path,content);}
 }
 if(stale.length)fail(`stale generated docs: ${stale.join(', ')}; run npm run docs:generate`);
 return result;
}
if(process.argv[1] && resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
 const flags=process.argv.slice(2);if(flags.some(x=>x!=='--check'))fail('usage: generate-docs.mjs [--check]');
 const root=fileURLToPath(new URL('../../',import.meta.url));
 try {await generateDocs(root,{check:flags.includes('--check')});console.log(flags.includes('--check')?'Documentation is synchronized.':'Generated public documentation.');}
 catch(error){console.error(error.message);process.exitCode=1;}
}
