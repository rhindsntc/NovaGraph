import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, mkdir, writeFile, readFile, rm, symlink, readdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {exampleMetadata} from '../scripts/example-metadata.mjs';
import {buildDocs, generateDocs} from '../scripts/generate-docs.mjs';

async function fixture(t) {
 const root=await mkdtemp(join(tmpdir(),'nova-docs-'));t.after(()=>rm(root,{recursive:true,force:true}));
 for(const dir of ['docs-web/content','tests','examples/snippets','docs-web/src/data/generated']) await mkdir(join(root,dir),{recursive:true});
 const site={schemaVersion:1,version:'development',reviewedRevision:'b238b5a0033b4d022f67ec06320026794840e429',categories:[{id:'start',title:'Getting Started'}],articles:[{id:'overview',categoryId:'start',source:'docs-web/content/overview.md',snippetIds:['sample']}],snippets:[{id:'sample',path:'examples/snippets/sample.swift',label:'Swift',language:'swift'}],presets:[]};
 await writeFile(join(root,'docs-web/content/site.json'),JSON.stringify(site));
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\nAn experimental database.\n\n## Details\n\n[Details](#details)\n');
 await writeFile(join(root,'examples/snippets/sample.swift'),'print("Nova")\n');
 await writeFile(join(root,'tests/catalog.json'),JSON.stringify({schemaVersion:1,tests:[{id:'cpp.sample',suite:'values',owner:'',language:'cpp'}]}));
 await writeFile(join(root,'docs-web/content/features.json'),JSON.stringify({schemaVersion:1,features:[{id:'SCOPE-001',title:'Embedded',status:'implemented',acceptance:'Runs in process.',testIds:['cpp.sample'],articleIds:['overview'],introducedVersion:null,ownerTasks:['T02']}]}));
 await writeFile(join(root,'examples/manifest.json'),JSON.stringify({schemaVersion:1,examples:[{id:'sample',path:'examples/snippets/sample.swift',language:'swift',mode:'fragment',setupGroup:'none',setup:[],platforms:['macOS'],limitations:'Illustrative fixture.'}]}));
 await writeFile(join(root,'examples/recorded-results.json'),JSON.stringify({schemaVersion:1,revision:site.reviewedRevision,...await exampleMetadata(root),outputs:{}}));
 return root;
}
async function edit(root,path,fn) {const p=join(root,path);const value=JSON.parse(await readFile(p,'utf8'));fn(value);await writeFile(p,JSON.stringify(value));}

test('canonical prose and snippets drive articles, search and stable outputs',async t=>{
 const root=await fixture(t);const first=await buildDocs(root);const second=await buildDocs(root);assert.deepEqual(first.outputs,second.outputs);
 const data=JSON.parse(first.outputs['docs-web/src/data/generated/docs.json']);
 assert.match(data.categories[0].articles[0].content,/experimental/);assert.match(data.search[0].preview,/experimental/);
 assert.equal(data.categories[0].articles[0].codeTabs[0].code,'print("Nova")\n');
 assert.deepEqual(Object.keys(first.outputs),['docs-web/src/data/generated/docs.json']);
});
test('check detects prose drift and edited output; generation restores parity',async t=>{
 const root=await fixture(t);await generateDocs(root);await generateDocs(root,{check:true});
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\nChanged release status.\n');
 await assert.rejects(generateDocs(root,{check:true}),/stale/);
 await generateDocs(root);const data=JSON.parse(await readFile(join(root,'docs-web/src/data/generated/docs.json'),'utf8'));
 assert.match(data.search[0].preview,/Changed release status/);await generateDocs(root,{check:true});
 await writeFile(join(root,'docs-web/src/data/generated/docs.json'),'{}');await assert.rejects(generateDocs(root,{check:true}),/stale/);
});
for(const [name,mutate,pattern] of [
 ['duplicate article',s=>s.articles.push({...s.articles[0]}),/duplicate/],
 ['missing snippet',s=>s.articles[0].snippetIds=['absent'],/snippet/],
 ['unknown category',s=>s.articles[0].categoryId='absent',/category/],
 ['escaping source',s=>s.articles[0].source='../outside.md',/outside|unsafe/],
]) test(`rejects ${name}`,async t=>{const root=await fixture(t);await edit(root,'docs-web/content/site.json',mutate);await assert.rejects(buildDocs(root),pattern);});
for(const [name,mutate,pattern] of [
 ['unknown test',f=>f.features[0].testIds=['cpp.absent'],/test/],
 ['unknown article',f=>f.features[0].articleIds=['absent'],/article/],
 ['invalid status',f=>f.features[0].status='complete',/status/],
 ['verification without evidence',f=>f.features[0].status='verified',/evidence/],
]) test(`rejects ${name}`,async t=>{const root=await fixture(t);await edit(root,'docs-web/content/features.json',mutate);await assert.rejects(buildDocs(root),pattern);});
test('validates local Markdown links and anchors, including cross-file links',async t=>{
 const root=await fixture(t);await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Missing](#missing)\n');await assert.rejects(buildDocs(root),/anchor/);
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Missing](absent.md)\n');await assert.rejects(buildDocs(root),/link/);
 await writeFile(join(root,'docs-web/content/other.md'),'# Other\n\n## Real anchor\n');
 await edit(root,'docs-web/content/site.json',s=>s.articles.push({id:'other',categoryId:'start',source:'docs-web/content/other.md',snippetIds:[]}));
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Good](other.md#real-anchor)\n');await buildDocs(root);
});
test('malformed metadata and missing source files fail loudly',async t=>{
 const root=await fixture(t);await edit(root,'docs-web/content/site.json',s=>s.articles[0].snippetIds='sample');await assert.rejects(buildDocs(root),/array/);
 await edit(root,'docs-web/content/site.json',s=>{s.articles[0].snippetIds=[];s.articles[0].source='docs-web/content/missing.md';});await assert.rejects(buildDocs(root),/missing|ENOENT/);
});
test('published links and duplicate heading anchors navigate within the web app',async t=>{
 const root=await fixture(t);
 await edit(root,'docs-web/content/site.json',s=>s.articles.push({id:'other',categoryId:'start',source:'docs-web/content/other.md',snippetIds:[]}));
 await writeFile(join(root,'docs-web/content/other.md'),'# Other\n\n## Details\n\nFirst\n\n## Details\n\nSecond\n');
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Other](other.md#details-1)\n\n[Local](#overview)\n');
 const data=JSON.parse((await buildDocs(root)).outputs['docs-web/src/data/generated/docs.json']);
 assert.match(data.categories[0].articles[0].html,/href="#other\/details-1"/);
 assert.match(data.categories[0].articles[0].html,/href="#overview\/overview"/);
 assert.match(data.categories[0].articles[1].html,/id="other\/details-1"/);
});
test('categories with the same label keep their own articles',async t=>{
 const root=await fixture(t);await edit(root,'docs-web/content/site.json',s=>s.categories.push({id:'second',title:'Getting Started'}));
 const data=JSON.parse((await buildDocs(root)).outputs['docs-web/src/data/generated/docs.json']);
 assert.equal(data.categories[1].articles.length,0);
});
test('rejects malformed test inventory records',async t=>{
 const root=await fixture(t);await edit(root,'tests/catalog.json',c=>c.tests[0].language='imaginary');
 await assert.rejects(buildDocs(root),/test.*language/);
});
test('rejects repository links that have no published destination',async t=>{
 const root=await fixture(t);await writeFile(join(root,'docs-web/content/unpublished.md'),'# Hidden\n');
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Hidden](unpublished.md)\n');
 await assert.rejects(buildDocs(root),/unpublished/);
});
test('section articles resolve their children, including repeated headings',async t=>{
 const root=await fixture(t);
 await edit(root,'docs-web/content/site.json',s=>s.articles.push({id:'section',categoryId:'start',source:'docs-web/content/section.md',anchor:'part',snippetIds:[]}));
 await writeFile(join(root,'docs-web/content/section.md'),'# Sections\n\n## Part\n\n[Child](#child)\n\n### Child\n\nFirst\n\n### Child\n\nSecond\n\n## Unpublished\n');
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Child](section.md#child)\n\n[Repeated](section.md#child-1)\n');
 const data=JSON.parse((await buildDocs(root)).outputs['docs-web/src/data/generated/docs.json']);
 assert.match(data.categories[0].articles[0].html,/href="#section\/child"/);
 assert.match(data.categories[0].articles[0].html,/href="#section\/child-1"/);
 assert.match(data.categories[0].articles[1].html,/href="#section\/child"/);
 assert.match(data.categories[0].articles[1].html,/id="section\/child-1"/);
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n[Hidden](section.md#unpublished)\n');
 await assert.rejects(buildDocs(root),/unpublished/);
});
test('a section can start at a repeated heading without changing its anchor',async t=>{
 const root=await fixture(t);
 await edit(root,'docs-web/content/site.json',s=>s.articles[0].anchor='part-1');
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n## Part\n\nFirst\n\n## Part\n\n[Self](#part-1)\n');
 const data=JSON.parse((await buildDocs(root)).outputs['docs-web/src/data/generated/docs.json']);
 assert.match(data.categories[0].articles[0].html,/href="#overview\/part-1"/);
 assert.match(data.categories[0].articles[0].html,/id="overview\/part-1"/);
 assert.doesNotMatch(data.categories[0].articles[0].html,/First/);
});

test('search includes section headings and body content, with real section routes', async t=>{
 const root=await fixture(t);
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\nIntro.\n\n## Memory management\n\nPressure reclamation releases payload bytes.\n');
 const data=JSON.parse((await buildDocs(root)).outputs['docs-web/src/data/generated/docs.json']);
 assert.ok(data.search.some(e=>e.articleId==='overview/memory-management' && e.content.includes('reclamation')));
});
test('invalid related article references are rejected', async t=>{
 const root=await fixture(t);await edit(root,'docs-web/content/site.json',s=>s.articles[0].relatedIds=['absent']);
 await assert.rejects(buildDocs(root),/related/);
});

test('recorded results reject input drift and cannot put fragments in runnable presets', async t=>{
 const root=await fixture(t);
 await writeFile(join(root,'examples/snippets/sample.swift'),'print("Changed")\n');
 await assert.rejects(buildDocs(root),/recorded.*stale/);
});
test('snippet code cannot silently diverge from its executable source', async t=>{
 const root=await fixture(t);await writeFile(join(root,'examples/snippets/other.swift'),'print("other")\n');
 await edit(root,'docs-web/content/site.json',s=>s.snippets[0].path='examples/snippets/other.swift');
 await assert.rejects(buildDocs(root),/snippet.*source/);
});

test('fragments and future syntax cannot become runnable presets', async t=>{
 const root=await fixture(t);await writeFile(join(root,'examples/graph.json'),'{"nodes":[],"edges":[]}');
 await edit(root,'docs-web/content/site.json',s=>s.presets=[{id:'fragment',name:'Future',description:'Incomplete',querySource:'examples/snippets/sample.swift',graphSource:'examples/graph.json',exampleId:'sample'}]);
 await assert.rejects(buildDocs(root),/runnable NGQL/);
});
test('changed engine source invalidates recorded example provenance', async t=>{
 const root=await fixture(t);await mkdir(join(root,'cpp/src'),{recursive:true});await writeFile(join(root,'cpp/src/changed.cpp'),'// changed engine\n');
 await assert.rejects(buildDocs(root),/recorded.*stale/);
});

test('published local screenshots resolve to served image assets', async t => {
 const root=await fixture(t);
 await mkdir(join(root,'docs-web/public/images'),{recursive:true});
 await writeFile(join(root,'docs-web/public/images/sample.png'),Buffer.from('89504e470d0a1a0a','hex'));
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n![Sample screen](../public/images/sample.png)\n');
 const built=await buildDocs(root);
 assert.match(built.outputs['docs-web/src/data/generated/docs.json'], /images\/sample.png/);
 assert.doesNotMatch(built.outputs['docs-web/src/data/generated/docs.json'], /src=\\"#overview/);
});

test('screenshots outside the public image directory stay unpublished', async t => {
 const root=await fixture(t);
 await writeFile(join(root,'docs-web/content/private.png'),'private');
 await writeFile(join(root,'docs-web/content/overview.md'),'# Overview\n\n![Private](private.png)\n');
 await assert.rejects(buildDocs(root), /unpublished local destination/);
});


test('public generation works without private docs and only writes the public data file', async t => {
 const root=await fixture(t);
 await edit(root,'docs-web/content/features.json',f=>f.features=[]);
 const built=await generateDocs(root);
 assert.deepEqual(Object.keys(built.outputs),['docs-web/src/data/generated/docs.json']);
 assert.deepEqual(JSON.parse(built.outputs['docs-web/src/data/generated/docs.json']).features,[]);
 assert.ok(!(await readdir(root)).includes('docs'));
 await generateDocs(root,{check:true});
});

for(const source of ['docs/private.md','docs-web/content/../../docs/private.md']) {
 test(`rejects private article source ${source}`, async t => {
  const root=await fixture(t);
  await mkdir(join(root,'docs'));
  await writeFile(join(root,'docs/private.md'),'# Private\n\nInternal release notes.\n');
  await edit(root,'docs-web/content/site.json',s=>s.articles[0].source=source);
  await assert.rejects(buildDocs(root),/private|public source/);
 });
}

test('rejects a public article symlink to private docs', async t => {
 const root=await fixture(t);
 await mkdir(join(root,'docs'));
 await writeFile(join(root,'docs/private.md'),'# Private\n');
 await symlink(join(root,'docs/private.md'),join(root,'docs-web/content/alias.md'));
 await edit(root,'docs-web/content/site.json',s=>s.articles[0].source='docs-web/content/alias.md');
 await assert.rejects(buildDocs(root),/private|public source/);
});

test('rejects a public metadata symlink to private docs', async t => {
 const root=await fixture(t);
 await mkdir(join(root,'docs'));
 await writeFile(join(root,'docs/site.json'),await readFile(join(root,'docs-web/content/site.json')));
 await rm(join(root,'docs-web/content/site.json'));
 await symlink(join(root,'docs/site.json'),join(root,'docs-web/content/site.json'));
 await assert.rejects(buildDocs(root),/private|public source/);
});

for(const flag of ['artifactMetadata','performanceMetadata']) {
 test(`rejects deprecated ${flag} instead of reading internal reports`, async t => {
  const root=await fixture(t);
  await edit(root,'docs-web/content/site.json',s=>s[flag]='docs/missing-private-report.json');
  await assert.rejects(buildDocs(root),new RegExp(`deprecated.*${flag}`));
 });
}

test('rejects example inputs in private docs before checking provenance', async t => {
 const root=await fixture(t);
 await edit(root,'examples/manifest.json',m=>m.examples[0].path='docs/private.swift');
 await assert.rejects(buildDocs(root),/private|public source/);
});

test('runnable public presets keep their recorded outputs and provenance', async t => {
 const root=await fixture(t);
 await writeFile(join(root,'examples/query.ngql'),'MATCH (n) RETURN n\n');
 await writeFile(join(root,'examples/expected.json'),'[{"name":"Nova"}]\n');
 await edit(root,'examples/manifest.json',m=>m.examples.push({id:'query',path:'examples/query.ngql',language:'ngql',mode:'run',setupGroup:'query',setup:[],platforms:['portable'],expected:'examples/expected.json'}));
 await edit(root,'tests/catalog.json',c=>c.tests.push({id:'docs.query',suite:'examples',owner:'',language:'docs'}));
 await edit(root,'docs-web/content/site.json',s=>s.presets=[{id:'query',name:'Query',description:'Recorded query',querySource:'examples/query.ngql',exampleId:'query'}]);
 const metadata=await exampleMetadata(root);
 await edit(root,'examples/recorded-results.json',r=>Object.assign(r,metadata,{outputs:{query:[{name:'Nova'}]}}));
 const data=JSON.parse((await buildDocs(root)).outputs['docs-web/src/data/generated/docs.json']);
 assert.equal(data.presets[0].query,'MATCH (n) RETURN n\n');
 assert.equal(data.presets[0].recorded.outputText,'[{"name":"Nova"}]\n');
 assert.equal(data.presets[0].recorded.revision,'b238b5a0033b4d022f67ec06320026794840e429');
});
