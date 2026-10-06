const required=['macos','ios','ios-simulator','catalyst','tvos','tvos-simulator','watchos','watchos-simulator'];
const names={macos:'macOS',ios:'iOS device','ios-simulator':'iOS simulator',catalyst:'Mac Catalyst',tvos:'tvOS device','tvos-simulator':'tvOS simulator',watchos:'watchOS device','watchos-simulator':'watchOS simulator'};
const architectureSets={ios:['arm64'],tvos:['arm64'],watchos:['arm64','arm64_32']};
const fail=message=>{throw new Error(message);};
const size=value=>Number.isSafeInteger(value)&&value>0?(value/1024).toFixed(2)+' KiB':fail('invalid artifact size');
export function renderAppleArtifacts(m) {
 if(m.schemaVersion!==1 || !/^[a-f0-9]{40}$/.test(m.revision) || !/^[a-f0-9]{64}$/.test(m.artifactContentDigest))fail('invalid artifact provenance');
 if(m.dirty!==false)fail('dirty candidate cannot supply documentation evidence');
 if(!Array.isArray(m.slices)||m.slices.length!==8||new Set(m.slices.map(r=>r.id)).size!==8||required.some(id=>!m.slices.some(r=>r.id===id)))fail('incomplete artifact matrix');
 if(m.publishedURL!==null && (typeof m.publishedURL!=='string'||!m.publishedURL.startsWith('https://')))fail('invalid download URL');
 const evidence=m.consumerEvidence;
 if(!evidence||evidence.dirty!==false||evidence.revision!==m.revision||evidence.artifactContentDigest!==m.artifactContentDigest)fail('stale artifact evidence');
 const runtime=evidence.runtimeEvidence;
 if(!Array.isArray(runtime)||runtime.some(r=>r.revision!==m.revision||r.artifactContentDigest!==m.artifactContentDigest))fail('stale runtime evidence');
 const rows=required.map(id=>m.slices.find(r=>r.id===id));
 for(const r of rows) {
  const expected=architectureSets[r.id]||['arm64','x86_64'];
  if(!Array.isArray(r.architectures)||JSON.stringify([...r.architectures].sort())!==JSON.stringify([...expected].sort()))fail('missing artifact architecture');
  if(r.buildStatus!=='verified')fail('artifact build is not verified');
  if(r.minimumOS!==({macos:'13.0',watchos:'9.0','watchos-simulator':'9.0'}[r.id]||'16.0'))fail('incorrect artifact deployment floor');
  if(r.id==='watchos' && (r.minimumOSByArchitecture?.arm64!=='26.0'||r.minimumOSByArchitecture?.arm64_32!=='9.0'))fail('incorrect watchOS architecture floor');
 }
 const qualified=r=>r.architectures.every(architecture=>['source','binary'].every(distribution=>runtime.some(e=>e.id===r.id&&e.architecture===architecture&&e.distribution===distribution&&e.status==='passed'&&e.check==='create-query-close-reopen'&&e.kind===(r.id.endsWith('simulator')?'simulator':['macos','catalyst'].includes(r.id)?'native':'device')&&e.osVersion.split('.').slice(0,2).join('.')===(r.id==='catalyst'?'13.0':r.minimumOSByArchitecture?.[architecture]||r.minimumOS))));
 const ready=rows.every(qualified);
 if(m.qualification.releaseReady!==ready)fail('qualification differs from minimum-runtime evidence');
 const floors=r=>r.architectures.map(a=>`${a}: ${r.minimumOSByArchitecture?.[a]||r.minimumOS}`).join(', ');
 const checked=r=>{const passed=runtime.filter(e=>e.id===r.id&&e.status==='passed');return passed.length?[...new Set(passed.map(e=>`${e.distribution} ${e.architecture} ${e.osVersion} (${e.kind})`))].join('; '):'No runtime run';};
 const table='| Platform | Architecture: minimum OS | Native archive / compressed | Consumer runtime evidence | Support gate |\n|---|---|---|---|---|\n'+rows.map(r=>`| ${names[r.id]} | ${floors(r)} | ${size(r.installedBytes)} / ${size(r.compressedBytes)} | ${checked(r)} | ${qualified(r)?'Minimum-runtime checks passed':'Unqualified'} |`).join('\n');
 const markdown='# Apple artifact matrix\n\nGenerated from the checked-in artifact manifest and matching consumer evidence. Nova remains experimental. Building a deployment target does not establish runtime support.\n\n'+
 `Candidate **${m.version}**, source revision \`${m.revision}\`. ${m.xcode.replaceAll('\n',', ')}.\n\n`+table+'\n\n'+
 'Sizes measure the native static library only, compressed independently with ZIP/DEFLATE level 9; wrapper sources and headers are included in the complete bundle. These are measurements, not size budgets (T21).\n\n'+
 '## Download and provenance\n\n'+`| Candidate archive | Download | SHA-256 |\n|---|---|---|\n| ${m.archive.name} | ${m.publishedURL?`[Download](${m.publishedURL})`:'Not published; local candidate only'} | \`${m.archive.sha256}\` |\n\n`+
 'The bundle contains a C-only XCFramework, GraphDBKit Swift sources, a SwiftPM manifest that supplies libc++ linkage, complete project/vendored license texts, scoped dependency inventory and per-file SHA-256 records. Swift consumers still compile the wrapper with their toolchain; no precompiled Swift ABI is promised. Archives are unsigned local candidates; hosted publication/signing remain release work.\n\n'+
 '## Remaining qualification\n\n'+(ready?'Minimum-runtime matrix passed; other production gates still apply.':m.qualification.blockers.map(b=>'- '+b).join('\n'))+'\n\nwatchOS arm64_32 retains the watchOS 9 floor; the current Apple SDK requires watchOS 26 for arm64. Catalyst 16 is an iOSMac deployment floor corresponding to a macOS 13 native runtime. No visionOS support is claimed. See [T19 implementation](T19_IMPLEMENTATION.md) for commands and validation limits.\n';
 return {markdown,platforms:rows.map(r=>({name:names[r.id],minimum:floors(r),status:qualified(r)?'minimum-runtime checks passed; experimental':'build verified; runtime support unqualified'}))};
}
