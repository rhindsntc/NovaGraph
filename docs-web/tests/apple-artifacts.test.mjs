import test from 'node:test';
import assert from 'node:assert/strict';
import {renderAppleArtifacts} from '../scripts/apple-artifacts.mjs';
const ids=['macos','ios','ios-simulator','catalyst','tvos','tvos-simulator','watchos','watchos-simulator'];
function candidate() {return {schemaVersion:1,version:'0.0.0-test',revision:'a'.repeat(40),dirty:false,artifactContentDigest:'b'.repeat(64),publishedURL:null,xcode:'Xcode 27',archive:{name:'NovaGraph-0.0.0-test.zip',sha256:'c'.repeat(64)},slices:ids.map(id=>({id,architectures:(['ios','tvos'].includes(id)?['arm64']:id==='watchos'?['arm64','arm64_32']:['arm64','x86_64']),minimumOS:(id==='macos'?'13.0':id.startsWith('watchos')?'9.0':'16.0'),minimumOSByArchitecture:(id==='watchos'?{arm64:'26.0',arm64_32:'9.0'}:{}),installedBytes:2000,compressedBytes:1000,buildStatus:'verified'})),qualification:{releaseReady:false,blockers:ids.map(id=>id+': minimum-runtime/device evidence unavailable')},consumerEvidence:{revision:'a'.repeat(40),artifactContentDigest:'b'.repeat(64),dirty:false,runtimeEvidence:[]}};}
test('artifact table keeps build and runtime qualification distinct',()=>{
 const r=renderAppleArtifacts(candidate());assert.match(r.markdown,/Unqualified/);assert.match(r.markdown,/Not published/);assert.match(r.markdown,/1.95 KiB/);assert.equal(r.platforms.length,8);assert.ok(r.platforms.every(p=>p.status.includes('unqualified')));
});
test('missing artifact cannot create a supported platform row',()=>{
 const m=candidate();m.slices.pop();assert.throws(()=>renderAppleArtifacts(m),/matrix/);
});
test('stale runtime evidence cannot appear in generated docs',()=>{
 const m=candidate();m.consumerEvidence.artifactContentDigest='f'.repeat(64);assert.throws(()=>renderAppleArtifacts(m),/evidence/);
});
test('a green label without minimum-runtime evidence is rejected',()=>{
 const m=candidate();m.qualification={releaseReady:true,blockers:[]};assert.throws(()=>renderAppleArtifacts(m),/qualification/);
});
test('invalid sizes and unsafe download URLs fail generation',()=>{
 const m=candidate();m.slices[0].installedBytes=-1;assert.throws(()=>renderAppleArtifacts(m),/size/);
 m.slices[0].installedBytes=1;m.publishedURL='javascript:alert(1)';assert.throws(()=>renderAppleArtifacts(m),/URL/);
});

test('missing architecture or an unverified build cannot earn a platform row',()=>{
 const m=candidate();m.slices[0].architectures=[];assert.throws(()=>renderAppleArtifacts(m),/architecture/);
 const n=candidate();n.slices[0].buildStatus='missing';assert.throws(()=>renderAppleArtifacts(n),/build/);
});
