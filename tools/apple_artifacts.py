#!/usr/bin/env python3
"""Build and inspect complete Apple C-ABI XCFramework candidates; never publish."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# Required baseline architectures, excluding optional arm64e/x86_64h subtypes.
SLICES = [
    dict(id='macos',sdk='macosx',target='macosx',platform='macos',variant='',architectures=['arm64','x86_64'],minimumOS='13.0',triple='{arch}-apple-macos13.0',platformID=1),
    dict(id='ios',sdk='iphoneos',target='iphoneos',platform='ios',variant='',architectures=['arm64'],minimumOS='16.0',triple='{arch}-apple-ios16.0',platformID=2),
    dict(id='ios-simulator',sdk='iphonesimulator',target='iphonesimulator',platform='ios',variant='simulator',architectures=['arm64','x86_64'],minimumOS='16.0',triple='{arch}-apple-ios16.0-simulator',platformID=7),
    dict(id='catalyst',sdk='macosx',target='iosmac',platform='ios',variant='maccatalyst',architectures=['arm64','x86_64'],minimumOS='16.0',triple='{arch}-apple-ios16.0-macabi',platformID=6),
    dict(id='tvos',sdk='appletvos',target='appletvos',platform='tvos',variant='',architectures=['arm64'],minimumOS='16.0',triple='{arch}-apple-tvos16.0',platformID=3),
    dict(id='tvos-simulator',sdk='appletvsimulator',target='appletvsimulator',platform='tvos',variant='simulator',architectures=['arm64','x86_64'],minimumOS='16.0',triple='{arch}-apple-tvos16.0-simulator',platformID=8),
    dict(id='watchos',sdk='watchos',target='watchos',platform='watchos',variant='',architectures=['arm64','arm64_32'],minimumOS='9.0',minimumOSByArchitecture={'arm64':'26.0','arm64_32':'9.0'},triple='{arch}-apple-watchos{minimum}',platformID=4),
    dict(id='watchos-simulator',sdk='watchsimulator',target='watchsimulator',platform='watchos',variant='simulator',architectures=['arm64','x86_64'],minimumOS='9.0',triple='{arch}-apple-watchos9.0-simulator',platformID=9),
]

def run(args, *, cwd=ROOT, env=None):
    result = subprocess.run([str(v) for v in args],cwd=cwd,env=env,text=True,capture_output=True)
    if result.returncode:
        raise ValueError(f'Command failed ({result.returncode}): {args}\n{result.stdout}\n{result.stderr}')
    return result.stdout.strip()

def sha256(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def confined(root, name):
    root=Path(root).resolve(); relative=Path(name)
    if relative.is_absolute() or '..' in relative.parts: raise ValueError('unsafe artifact path')
    path=(root/relative).resolve()
    if not path.is_relative_to(root): raise ValueError('artifact path escapes bundle')
    return path

def validate_files(root, records):
    if not records: raise ValueError('missing file inventory')
    actual={p.relative_to(root).as_posix() for p in Path(root).rglob('*') if p.is_file() and p.relative_to(root).as_posix()!='artifact-manifest.json'}
    if actual!=set(records): raise ValueError('artifact file inventory differs from bundle')
    for name,digest in records.items():
        path=confined(root,name)
        if not path.is_file() or sha256(path)!=digest: raise ValueError(f'missing or changed artifact: {name}')

def validate_matrix(rows):
    expected={s['id']:s for s in SLICES}
    if len(rows)!=len(expected) or set(r['id'] for r in rows)!=set(expected): raise ValueError('incomplete or duplicate Apple matrix')
    for row in rows:
        wanted=expected[row['id']]
        for key in ('platform','variant','minimumOS'):
            if row.get(key)!=wanted[key]: raise ValueError(f'{row["id"]}: incorrect {key}')
        if row.get('minimumOSByArchitecture',{})!=wanted.get('minimumOSByArchitecture',{}): raise ValueError(f'{row["id"]}: incorrect architecture deployment floors')
        if sorted(row.get('architectures',[]))!=sorted(wanted['architectures']): raise ValueError(f'{row["id"]}: missing or unexpected architecture')

def validate_load_commands(output, platform, minimum, objects):
    blocks=re.findall(r'cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform (\S+)\s+minos ([\d.]+)',output)
    if len(blocks)!=objects: raise ValueError(f'expected {objects} object platform records, got {len(blocks)}')
    names={'MACOS':1,'IOS':2,'TVOS':3,'WATCHOS':4,'MACCATALYST':6,'IOSSIMULATOR':7,'TVOSSIMULATOR':8,'WATCHOSSIMULATOR':9}
    for actual,floor in blocks:
        number=int(actual) if actual.isdigit() else names.get(actual)
        if number!=platform or tuple(map(int,floor.split('.')))!=tuple(map(int,minimum.split('.'))):
            raise ValueError(f'object platform/minimum {actual}/{floor} differs from {platform}/{minimum}')

def qualification(rows, runtime):
    blockers=[]
    for row in rows:
        kind='simulator' if row['variant']=='simulator' else ('native' if row['id'] in ('macos','catalyst') else 'device')
        for arch in row['architectures']:
            floor='13.0' if row['id']=='catalyst' else row.get('minimumOSByArchitecture',{}).get(arch,row['minimumOS'])
            for distribution in ('source','binary'):
                matches=[r for r in runtime if r.get('id')==row['id'] and r.get('architecture')==arch and r.get('distribution')==distribution and r.get('status')=='passed' and r.get('kind')==kind and '.'.join(r.get('osVersion','').split('.')[:2])==floor and r.get('check')=='create-query-close-reopen']
                if not matches:
                    blockers.append(f'{row["id"]}: minimum-runtime/device evidence unavailable')
                    break
            else: continue
            break
    return dict(releaseReady=not blockers,blockers=blockers)

def content_digest(manifest):
    return hashlib.sha256(json.dumps(manifest['files'],sort_keys=True).encode()).hexdigest()

def validate_evidence(manifest, report, complete=True):
    if report.get('dirty') is not False or manifest.get('dirty') is not False:
        raise ValueError('dirty candidates cannot supply release evidence')
    if report.get('sourceRevision')!=manifest['revision']:raise ValueError('consumer source revision differs from candidate')
    if report.get('revision')!=manifest['revision'] or report.get('artifactContentDigest')!=content_digest(manifest):
        raise ValueError('consumer evidence belongs to another candidate')
    rows={s['id']:s for s in manifest['slices']}
    expected={(i,d) for i in rows for d in ('source','binary')}
    builds=report.get('builds',[])
    if complete and (len(builds)!=len(expected) or {(r['id'],r['distribution']) for r in builds}!=expected):
        raise ValueError('incomplete source/binary consumer build evidence')
    for b in builds:
        if (b.get('id'),b.get('distribution')) not in expected:raise ValueError('unknown consumer build')
        if b.get('status')!='passed' or sorted(b['architectures'])!=sorted(rows[b['id']]['architectures']):
            raise ValueError('consumer build failed or missing architecture')
    for r in report.get('runtimeEvidence',[]):
        if r.get('revision')!=manifest['revision'] or r.get('artifactContentDigest')!=content_digest(manifest):
            raise ValueError('runtime evidence belongs to another candidate')
    return qualification(manifest['slices'],report.get('runtimeEvidence',[]))

def merge_evidence(manifest, reports):
    merged=dict(schemaVersion=1,revision=manifest['revision'],sourceRevision=manifest['revision'],artifactContentDigest=content_digest(manifest),dirty=False,builds=[],runtimeEvidence=[],unavailable=[])
    builds={}
    for report in reports:
        validate_evidence(manifest,report,complete=False)
        for b in report['builds']:
            key=(b['id'],b['distribution'])
            if key in builds and sorted(builds[key]['architectures'])!=sorted(b['architectures']):raise ValueError('conflicting consumer evidence')
            builds[key]=b
        merged['runtimeEvidence']+=report.get('runtimeEvidence',[])
        merged['unavailable']+=report.get('unavailable',[])
    merged['builds']=list(builds.values());merged['unavailable']=sorted(set(merged['unavailable']))
    merged['qualification']=validate_evidence(manifest,merged)
    return merged

def dependency_inventory():
    inventory=[dict(name='NovaGraph / GraphDBKit',license='Apache-2.0',scope='distributed engine and wrapper',licenseFile='LICENSE'),dict(name='nlohmann/json',version='3.12.0',license='MIT',scope='distributed private native header',licenseFile='LICENSE.nlohmann-json',sourceSha256=sha256(ROOT/'cpp/third_party/nlohmann/json.hpp'))]
    swift=json.loads((ROOT/'licenses/dependencies.json').read_text())
    pins=json.loads((ROOT/'devtools/Package.resolved').read_text())['pins']
    for pin in pins:
        row=next((r for r in swift if r['name']==pin['identity']),None)
        if row is None or row['revision']!=pin['state']['revision'] or row['version']!=pin['state']['version']:raise ValueError('stale Swift dependency/license inventory')
        inventory.append(row)
    for path,package in json.loads((ROOT/'docs-web/package-lock.json').read_text())['packages'].items():
        if not path:continue
        if not package.get('license'):raise ValueError('missing npm license: '+path)
        inventory.append(dict(name=path.split('node_modules/')[-1],version=package['version'],license=package['license'],scope='docs website / development only; not in core bundle',integrity=package.get('integrity'),path=path))
    return inventory

def require_qualified(gate):
    if not gate.get('releaseReady'): raise ValueError('release blocked: '+'; '.join(gate.get('blockers',[])))

def archive_tree(source, destination):
    source=Path(source)
    with zipfile.ZipFile(destination,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for path in sorted(source.rglob('*')):
            if path.is_symlink(): raise ValueError('symlink in archive')
            if path.is_file():
                info=zipfile.ZipInfo(source.name+'/'+path.relative_to(source).as_posix(),date_time=(1980,1,1,0,0,0))
                info.compress_type=zipfile.ZIP_DEFLATED;info.external_attr=(0o100644 << 16)
                archive.writestr(info,path.read_bytes())

def validate_archive(bundle, archive):
    bundle=Path(bundle)
    expected={bundle.name+'/'+p.relative_to(bundle).as_posix():p for p in bundle.rglob('*') if p.is_file()}
    with zipfile.ZipFile(archive) as z:
        if len(z.namelist())!=len(expected) or set(z.namelist())!=set(expected):raise ValueError('ZIP contents differ from verified bundle')
        for name,path in expected.items():
            info=z.getinfo(name)
            if info.file_size!=path.stat().st_size or hashlib.sha256(z.read(name)).hexdigest()!=sha256(path):raise ValueError('ZIP payload differs from verified bundle')

RESOURCE_SIZE_SCOPE = ('Logical regular-file bytes in the verified extracted bundle, excluding '
                       'artifact-manifest.json; fileCount uses the same exclusion. archiveBytes '
                       'is the complete verified ZIP including artifact-manifest.json. Slice sizes '
                       'are native static archives and independent ZIP/DEFLATE archives, not linked '
                       'or installed application size. No runtime qualification is implied.')

def validate_resource_sizes(manifest, sizes):
    """Validate recorded size evidence against its clean candidate's hash inventory."""
    if not isinstance(sizes,dict) or type(sizes.get('schemaVersion')) is not int or sizes['schemaVersion']!=1:
        raise ValueError('missing or unsupported resource size schema')
    revision=manifest.get('revision')
    if not isinstance(revision,str) or not re.fullmatch(r'[0-9a-f]{40}',revision):
        raise ValueError('resource sizes require a complete source revision')
    if manifest.get('dirty') is not False or sizes.get('dirty') is not False:
        raise ValueError('resource sizes require a clean candidate')
    records=manifest.get('files')
    if not isinstance(records,dict) or not records:raise ValueError('missing file inventory')
    if sizes.get('revision')!=revision or sizes.get('artifactContentDigest')!=content_digest(manifest):
        raise ValueError('resource sizes belong to another candidate')
    if sizes.get('scope')!=RESOURCE_SIZE_SCOPE:raise ValueError('unsupported resource size scope')
    for key in ('bundleLogicalBytes','archiveBytes','fileCount'):
        if type(sizes.get(key)) is not int or sizes[key]<=0:raise ValueError('missing or invalid resource size: '+key)
    if not isinstance(sizes.get('archiveSha256'),str) or not re.fullmatch(r'[0-9a-f]{64}',sizes['archiveSha256']):
        raise ValueError('missing or invalid resource archive hash')
    files=sizes.get('files')
    if not isinstance(files,dict) or set(files)!=set(records):raise ValueError('resource size file inventory differs from candidate')
    for name,digest in records.items():
        if not isinstance(name,str) or not name or Path(name).is_absolute() or '..' in Path(name).parts or Path(name).as_posix()!=name or name=='artifact-manifest.json':
            raise ValueError('unsafe resource size inventory path')
        if not isinstance(digest,str) or not re.fullmatch(r'[0-9a-f]{64}',digest):raise ValueError('invalid artifact file hash')
        measured=files[name]
        if not isinstance(measured,dict) or measured.get('sha256')!=digest or type(measured.get('bytes')) is not int or measured['bytes']<0:
            raise ValueError('resource size file metadata differs from candidate')
    if sizes['fileCount']!=len(files) or sizes['bundleLogicalBytes']!=sum(row['bytes'] for row in files.values()):
        raise ValueError('resource size totals differ from file measurements')
    rows=manifest.get('slices')
    if not isinstance(rows,list):raise ValueError('missing slice size inventory')
    expected=[]
    for row in rows:
        if not isinstance(row,dict) or not isinstance(row.get('id'),str):raise ValueError('invalid slice size identity')
        for key in ('installedBytes','compressedBytes'):
            if type(row.get(key)) is not int or row[key]<=0:raise ValueError('missing or invalid slice size: '+key)
        matches=[files[name]['bytes'] for name,digest in records.items() if digest==row.get('sha256')]
        if not matches or any(size!=row['installedBytes'] for size in matches):
            raise ValueError('slice size/hash differs from measured file inventory')
        expected.append({key:row[key] for key in ('id','installedBytes','compressedBytes','sha256')})
    if sizes.get('slices')!=expected:raise ValueError('resource slice sizes differ from candidate')
    if 'archive' in manifest:
        archive=manifest['archive']
        if not isinstance(archive,dict) or archive.get('bytes')!=sizes['archiveBytes'] or archive.get('sha256')!=sizes['archiveSha256']:
            raise ValueError('resource ZIP sizes differ from verified archive metadata')

def resource_size_report(bundle, manifest, archive):
    """Measure a verified bundle/ZIP again; do not infer or modify qualification.

    Call after verify_bundle for full platform/ABI validation. Revalidate source
    provenance, every file hash, ZIP contents and SHA256SUMS at this measurement
    boundary. Keep this report outside the bundle to avoid self-referential sizes.
    """
    bundle=Path(bundle).resolve();archive=Path(archive).resolve()
    stored=json.loads((bundle/'artifact-manifest.json').read_text())
    for key in ('revision','dirty','files','slices'):
        if key not in manifest or stored.get(key)!=manifest[key]:raise ValueError('resource candidate differs from bundled manifest')
    if any(path.is_symlink() for path in bundle.rglob('*')):raise ValueError('symlink in resource bundle')
    validate_files(bundle,manifest['files'])
    validate_archive(bundle,archive)
    archive_hash=sha256(archive)
    if (archive.parent/'SHA256SUMS').read_text()!=archive_hash+'  '+archive.name+'\n':
        raise ValueError('archive checksum differs from SHA256SUMS')
    files={name:dict(bytes=confined(bundle,name).stat().st_size,sha256=digest) for name,digest in manifest['files'].items()}
    sizes=dict(schemaVersion=1,revision=manifest['revision'],dirty=manifest['dirty'],
               artifactContentDigest=content_digest(manifest),bundleLogicalBytes=sum(row['bytes'] for row in files.values()),
               archiveBytes=archive.stat().st_size,archiveSha256=archive_hash,fileCount=len(files),
               files=files,scope=RESOURCE_SIZE_SCOPE,
               slices=[{key:row.get(key) for key in ('id','installedBytes','compressedBytes','sha256')} for row in manifest['slices']])
    validate_resource_sizes(manifest,sizes)
    return sizes

def validate_slice_sizes(library, row):
    """Check both the native archive and its independently compressed ZIP size."""
    for key in ('installedBytes','compressedBytes'):
        if type(row.get(key)) is not int or row[key]<=0:raise ValueError('missing or invalid slice size: '+key)
    if sha256(library)!=row.get('sha256') or Path(library).stat().st_size!=row['installedBytes']:
        raise ValueError('slice size/hash metadata differs from archive')
    compressed=io.BytesIO()
    with zipfile.ZipFile(compressed,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        z.write(library,'libnovagraphdb.a')
    if len(compressed.getbuffer())!=row['compressedBytes']:
        raise ValueError('slice compressed size metadata differs from archive')

def normalize_framework_info(path):
    path=Path(path);info=plistlib.loads(path.read_bytes())
    info['AvailableLibraries'].sort(key=lambda item:item['LibraryIdentifier'])
    path.write_bytes(plistlib.dumps(info,sort_keys=True))

def abi_symbols(header):
    return set(re.findall(r'\b(graphdb_[a-zA-Z0-9_]+)\s*\(',header))

def inspect_library(path, row, symbols, object_count):
    archs=run(['xcrun','lipo','-archs',path]).split()
    if sorted(archs)!=sorted(row['architectures']): raise ValueError(f'{path}: incorrect architectures {archs}')
    for arch in archs:
        output=run(['xcrun','otool','-arch',arch,'-l',path])
        validate_load_commands(output,row['platformID'],row.get('minimumOSByArchitecture',{}).get(arch,row['minimumOS']),object_count)
        exported=run(['xcrun','nm','-arch',arch,'-gUj',path]).splitlines()
        present={line.strip().lstrip('_') for line in exported}
        if not symbols.issubset(present): raise ValueError(f'{row["id"]}/{arch}: missing C exports {sorted(symbols-present)}')

def verify_bundle(bundle):
    bundle=Path(bundle).resolve();m=json.loads((bundle/'artifact-manifest.json').read_text())
    if m.get('schemaVersion')!=1: raise ValueError('unsupported artifact schema')
    validate_matrix(m['slices']);validate_files(bundle,m['files'])
    digest=content_digest(m)
    for result in m.get('runtimeEvidence',[]):
        if result.get('revision')!=m['revision'] or result.get('artifactContentDigest')!=digest: raise ValueError('runtime evidence belongs to another candidate')
    if m['qualification']!=qualification(m['slices'],m.get('runtimeEvidence',[])): raise ValueError('qualification summary differs from runtime evidence')
    framework=bundle/'Artifacts/NovaGraphCore.xcframework'
    info=plistlib.loads((framework/'Info.plist').read_bytes())['AvailableLibraries']
    if len(info)!=len(SLICES): raise ValueError('XCFramework is missing required slices')
    canonical=(bundle/'CGraphDB.h').read_text();symbols=abi_symbols(canonical)
    if not symbols: raise ValueError('missing C ABI')
    for row in m['slices']:
        entries=[entry for entry in info if entry['SupportedPlatform']==row['platform'] and entry.get('SupportedPlatformVariant','')==row['variant']]
        if len(entries)!=1: raise ValueError('incorrect XCFramework platform entries')
        entry=entries[0]
        if sorted(entry['SupportedArchitectures'])!=sorted(row['architectures']): raise ValueError('incorrect XCFramework architecture metadata')
        slice_root=confined(framework,entry['LibraryIdentifier'])
        headers=confined(slice_root,entry['HeadersPath'])
        if sorted(p.relative_to(headers).as_posix() for p in headers.rglob('*') if p.is_file())!=['CGraphDB.h','module.modulemap']: raise ValueError('private or missing public headers')
        if (headers/'CGraphDB.h').read_text()!=canonical: raise ValueError('C ABI header differs across slices')
        if (headers/'module.modulemap').read_text()!='module CGraphDB {\n  header "CGraphDB.h"\n  export *\n}\n': raise ValueError('invalid C module map')
        spec=next(s for s in SLICES if s['id']==row['id'])
        library=confined(slice_root,entry['LibraryPath'])
        validate_slice_sizes(library,row)
        inspect_library(library,spec,symbols,m['objectCount'])
    return m

def sdk_inventory():
    result={}
    for row in SLICES:
        sdk=row['sdk']
        if sdk not in result:
            path=Path(run(['xcrun','--sdk',sdk,'--show-sdk-path']))
            result[sdk]=dict(path=str(path),version=run(['xcrun','--sdk',sdk,'--show-sdk-version']),build=run(['xcrun','--sdk',sdk,'--show-sdk-build-version']),settings=json.loads((path/'SDKSettings.json').read_text()))
        target=result[sdk]['settings']['SupportedTargets'].get(row['target'])
        if not target or not set(row['architectures']).issubset(target['Archs']): raise ValueError(f'SDK missing required {row["id"]} architectures')
    return result

def build_input_digest():
    """Hash actual compiler, distributed wrapper/license, and manifest inputs.

    Include path names so additions/removals also change identity. Git's tree
    alone cannot identify --allow-dirty development inputs or untracked headers.
    """
    paths=[]
    for folder in ('cpp/src','cpp/include','cpp/third_party','swift/Sources/GraphDBKit'):
        paths.extend(path for path in (ROOT/folder).rglob('*') if path.is_file())
    paths.extend(ROOT/name for name in ('LICENSE','tools/apple_artifacts.py',
      'licenses/dependencies.json','devtools/Package.resolved','docs-web/package-lock.json'))
    digest=hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(ROOT).as_posix().encode()+b'\0'+path.read_bytes()+b'\0')
    return digest.hexdigest()

def build_source_identity():
    return dict(revision=run(['git','rev-parse','HEAD']),
                sourceTree=run(['git','rev-parse','HEAD^{tree}']),
                dirty=bool(run(['git','status','--porcelain'])),sourceDigest=build_input_digest())

def build(args):
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?',args.version): raise ValueError('version must be a semantic version')
    output=Path(args.output).resolve()
    if output.exists(): raise ValueError('output already exists; choose a new candidate directory')
    source_identity=build_source_identity()
    if source_identity['dirty'] and not args.allow_dirty: raise ValueError('committed clean sources required; --allow-dirty is development-only')
    xcode=run(['xcodebuild','-version'])
    if args.xcode_build and f'Build version {args.xcode_build}' not in xcode: raise ValueError('selected Xcode does not match pinned build')
    sdks=sdk_inventory()  # Fail before creating any destination if an SDK is absent.
    license_text=(ROOT/'LICENSE').read_text()
    if 'Generated scaffold' in license_text or 'END OF TERMS AND CONDITIONS' not in license_text: raise ValueError('complete project license required')
    sources=sorted((ROOT/'cpp/src').glob('*.cpp'))
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.nova-build-',dir=output.parent) as temp:
        work=Path(temp);bundle=work/f'NovaGraph-{args.version}';bundle.mkdir()
        headers=work/'Headers';headers.mkdir()
        shutil.copy2(ROOT/'cpp/include/graphdb/CGraphDB.h',headers/'CGraphDB.h')
        (headers/'module.modulemap').write_text('module CGraphDB {\n  header "CGraphDB.h"\n  export *\n}\n')
        symbols=abi_symbols((headers/'CGraphDB.h').read_text())
        compiler=run(['xcrun','--find','clang++']);env=dict(os.environ,ZERO_AR_DATE='1')
        rows=[];framework_args=['xcodebuild','-create-xcframework']
        for row in SLICES:
            print(f'Building {row["id"]}: {", ".join(row["architectures"])}',flush=True)
            slice_dir=work/row['id'];slice_dir.mkdir();thin=[]
            for arch in row['architectures']:
                objects=slice_dir/arch;objects.mkdir();triple=row['triple'].format(arch=arch,minimum=row.get('minimumOSByArchitecture',{}).get(arch,row['minimumOS']))
                def compile_one(source):
                    obj=objects/(source.stem+'.o')
                    run([compiler,'-target',triple,'-isysroot',sdks[row['sdk']]['path'],'-std=c++20','-O2','-DNDEBUG','-fPIC',f'-ffile-prefix-map={ROOT}=.', '-I'+str(ROOT/'cpp/include'),'-I'+str(ROOT/'cpp/third_party'),'-c',source,'-o',obj],env=env)
                    return obj
                with ThreadPoolExecutor(max_workers=args.jobs) as pool: object_files=list(pool.map(compile_one,sources))
                archive=objects/'libnovagraphdb.a';run(['xcrun','libtool','-static','-D','-o',archive,*object_files],env=env);thin.append(archive)
            library=slice_dir/'libnovagraphdb.a'
            if len(thin)==1: shutil.copy2(thin[0],library)
            else: run(['xcrun','lipo','-create',*thin,'-output',library])
            inspect_library(library,row,symbols,len(sources))
            compressed=work/(row['id']+'.zip')
            with zipfile.ZipFile(compressed,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:z.write(library,'libnovagraphdb.a')
            rows.append({k:row[k] for k in ('id','platform','variant','architectures','minimumOS')}|dict(minimumOSByArchitecture=row.get('minimumOSByArchitecture',{}),installedBytes=library.stat().st_size,compressedBytes=compressed.stat().st_size,sha256=sha256(library),sdk=sdks[row['sdk']]['version'],buildStatus='verified',runtimeStatus='unqualified'))
            framework_args+=['-library',str(library),'-headers',str(headers)]
        artifacts=bundle/'Artifacts';artifacts.mkdir();framework=artifacts/'NovaGraphCore.xcframework'
        run(framework_args+['-output',framework])
        normalize_framework_info(framework/'Info.plist')
        shutil.copytree(ROOT/'swift/Sources/GraphDBKit',bundle/'Sources/GraphDBKit')
        shutil.copy2(headers/'CGraphDB.h',bundle/'CGraphDB.h')
        shutil.copy2(ROOT/'LICENSE',bundle/'LICENSE')
        shutil.copy2(ROOT/'cpp/third_party/nlohmann/LICENSE.MIT',bundle/'LICENSE.nlohmann-json')
        (bundle/'Package.swift').write_text('// swift-tools-version: 6.0\nimport PackageDescription\nlet package = Package(name: "NovaGraph", platforms: [.macOS(.v13), .iOS(.v16), .tvOS(.v16), .watchOS(.v9), .macCatalyst(.v16)], products: [.library(name: "GraphDBKit", targets: ["GraphDBKit"])], targets: [.binaryTarget(name: "CGraphDB", path: "Artifacts/NovaGraphCore.xcframework"), .target(name: "GraphDBKit", dependencies: ["CGraphDB"], linkerSettings: [.linkedLibrary("c++")])])\n')
        manifest=dict(schemaVersion=1,version=args.version,**source_identity,xcode=xcode,clang=run([compiler,'--version']),swift=run(['swift','--version']),objectCount=len(sources),slices=rows,signing='unsigned static-library candidate',publishedURL=None,runtimeEvidence=[],qualification=qualification(rows,[]),dependencies=dependency_inventory())
        manifest['sdkBuilds']={key:{k:v[k] for k in ('version','build')} for key,v in sdks.items()}
        manifest['files']={p.relative_to(bundle).as_posix():sha256(p) for p in sorted(bundle.rglob('*')) if p.is_file()}
        (bundle/'artifact-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        verify_bundle(bundle)
        destination=work/'distribution';destination.mkdir();shutil.move(bundle,destination/bundle.name)
        archive=destination/(bundle.name+'.zip');archive_tree(destination/bundle.name,archive)
        (destination/'SHA256SUMS').write_text(sha256(archive)+'  '+archive.name+'\n')
        if build_source_identity()!=source_identity:
            raise ValueError('source identity changed during build; candidate was not published')
        destination.rename(output)
    print(f'Complete candidate: {output}; runtime/release qualification remains separate.')

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='command',required=True)
    b=sub.add_parser('build');b.add_argument('--version',required=True);b.add_argument('--output',required=True);b.add_argument('--jobs',type=int,default=4);b.add_argument('--allow-dirty',action='store_true');b.add_argument('--xcode-build')
    v=sub.add_parser('verify');v.add_argument('bundle');v.add_argument('--require-qualified',action='store_true');v.add_argument('--evidence',type=Path,action='append');v.add_argument('--report',type=Path)
    args=parser.parse_args()
    try:
        if args.command=='build':
            if args.jobs<1 or args.jobs>32: raise ValueError('jobs must be between 1 and 32')
            build(args)
        else:
            manifest=verify_bundle(args.bundle)
            if args.evidence:
                report=merge_evidence(manifest,[json.loads(path.read_text()) for path in args.evidence]);manifest['qualification']=report['qualification'];manifest['consumerEvidence']=report
            if args.require_qualified:
                if manifest['dirty']: raise ValueError('dirty candidates cannot be released')
                require_qualified(manifest['qualification'])
            if args.report:
                bundle=Path(args.bundle).resolve()
                archive=bundle.parent/(bundle.name+'.zip')
                report_path=args.report.resolve()
                if report_path.is_relative_to(bundle) or report_path in (archive,bundle.parent/'SHA256SUMS'):
                    raise ValueError('resource report must not modify the verified bundle, ZIP or checksum')
                sizes=resource_size_report(bundle,manifest,archive)
                manifest['archive']=dict(name=archive.name,sha256=sizes['archiveSha256'],bytes=sizes['archiveBytes'])
                manifest['artifactContentDigest']=sizes['artifactContentDigest']
                manifest['resourceSizes']=sizes
                args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(manifest,indent=2)+'\n')
            print(f'Verified {len(manifest["slices"])} required slices and all recorded file hashes.')
    except (ValueError,KeyError,OSError,zipfile.BadZipFile) as error: parser.exit(1,str(error)+'\n')

if __name__=='__main__': main()
