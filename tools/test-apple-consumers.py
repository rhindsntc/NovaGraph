#!/usr/bin/env python3
"""Build fresh source/binary Apple consumers and collect actual runtime evidence.

The candidate is never modified. Evidence binds to its revision and content digest.
Device checks are opt-in and use a disposable, development-signed iOS app.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid
from apple_artifacts import ROOT, SLICES, verify_bundle, qualification

DESTINATIONS={'macos':'macOS','ios':'iOS','ios-simulator':'iOS Simulator','catalyst':'macOS,variant=Mac Catalyst','tvos':'tvOS','tvos-simulator':'tvOS Simulator','watchos':'watchOS','watchos-simulator':'watchOS Simulator'}
PLATFORMS='[.macOS(.v13), .iOS(.v16), .tvOS(.v16), .watchOS(.v9), .macCatalyst(.v16)]'

def source_state(manifest, revision, dirty, development):
    return dict(sourceRevision=revision,dirty=bool(dirty or manifest['dirty'] or revision!=manifest['revision'] or development))

def select_runtime(runtimes, family, minimum, requested=None):
    available=[r for r in runtimes if r.get('isAvailable') and r['name'].startswith(family+' ')]
    if requested:
        matches=[r for r in available if r['identifier']==requested]
        if len(matches)!=1:raise ValueError(f'requested {family} simulator runtime is unavailable: {requested}')
        return matches[0]
    minimums=[r for r in available if '.'.join(r['version'].split('.')[:2])==minimum]
    candidates=minimums or available
    return sorted(candidates,key=lambda r:tuple(map(int,r['version'].split('.'))))[-1] if candidates else None

def run(command,cwd,log,timeout=900):
    result=subprocess.run([str(v) for v in command],cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
    Path(log).write_text(result.stdout)
    if result.returncode: raise ValueError(f'command failed ({result.returncode}); see {log}\n'+result.stdout[-2500:])
    return result.stdout

def smoke(command,cwd,logs,database=None):
    for mode in ('create','reopen'):
        output=run(command+[mode]+([str(database)] if database else []),cwd,logs/f'{mode}.log',timeout=120)
        if f'IntegrationSmoke {mode}: values, query and path passed' not in output:
            raise ValueError(f'{mode} did not produce the success marker; see {logs}')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('bundle',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--platforms',nargs='+',choices=list(DESTINATIONS),default=list(DESTINATIONS))
    p.add_argument('--simulator-runtime',action='append',default=[],metavar='FAMILY=IDENTIFIER',help='Select an installed ios/tvos/watchos runtime; default prefers the declared minimum, otherwise newest')
    p.add_argument('--device',help='Opt-in connected physical iPhone identifier')
    p.add_argument('--team',help='Existing Apple development signing team for --device')
    p.add_argument('--allow-dirty',action='store_true',help='Development only; source may differ from candidate')
    args=p.parse_args();bundle=args.bundle.resolve();output=args.output.resolve()
    m=verify_bundle(bundle)
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip())
    if (dirty or m['dirty'] or revision!=m['revision']) and not args.allow_dirty: p.error('clean source revision must match candidate')
    selected={}
    for value in args.simulator_runtime:
        family,separator,identifier=value.partition('=')
        if not separator or family not in ('ios','tvos','watchos') or not identifier or family in selected:p.error('use one FAMILY=IDENTIFIER per ios/tvos/watchos family')
        selected[family]=identifier
    state=source_state(m,revision,dirty,args.allow_dirty)
    if args.device and not args.team:p.error('--device requires an explicit development --team')
    if output.exists():p.error('output must be a new directory (no cached acceptance)')
    output.mkdir(parents=True);evidence=[];builds=[];unavailable=[]
    digest=hashlib.sha256(json.dumps(m['files'],sort_keys=True).encode()).hexdigest()
    def record(row,arch,distribution,kind,version):
        evidence.append(dict(id=row['id'],architecture=arch,distribution=distribution,kind=kind,osVersion=version,status='passed',check='create-query-close-reopen',revision=m['revision'],artifactContentDigest=digest))
    def save():
        report=dict(schemaVersion=1,revision=m['revision'],artifactContentDigest=digest,**state,builds=builds,runtimeEvidence=evidence,unavailable=unavailable,qualification=qualification(m['slices'],evidence))
        (output/'evidence.json').write_text(json.dumps(report,indent=2)+'\n')
    source=(ROOT/'examples/IntegrationSmoke/Sources/IntegrationSmoke/main.swift').read_text()
    # App sandbox supplies its Documents directory; command-line probes keep explicit paths.
    app_source=source.replace('guard CommandLine.arguments.count == 3 else', 'guard CommandLine.arguments.count >= 2 else').replace('let db = try GraphDatabase(path: URL(fileURLWithPath: CommandLine.arguments[2]))','let databaseURL = CommandLine.arguments.count == 3 ? URL(fileURLWithPath: CommandLine.arguments[2]) : FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("NovaArtifactSmoke")\nlet db = try GraphDatabase(path: databaseURL)')
    simulators={};owned=[]
    try:
        runtimes=json.loads(run(['xcrun','simctl','list','runtimes','-j'],ROOT,output/'runtimes.log'))['runtimes']
        types=json.loads(run(['xcrun','simctl','list','devicetypes','-j'],ROOT,output/'device-types.log'))['devicetypes']
        for family in ('iOS','tvOS','watchOS'):
            if family.lower()+'-simulator' not in args.platforms:continue
            minimum=next(r['minimumOS'] for r in SLICES if r['id']==family.lower()+'-simulator')
            runtime=select_runtime(runtimes,family,minimum,selected.get(family.lower()))
            if not runtime:
                unavailable.append(f'{family}: no installed simulator runtime');continue
            supported={t['identifier'] for t in runtime.get('supportedDeviceTypes',[])}
            candidates=[t for t in types if t['identifier'] in supported and ((family=='iOS' and t['name'].startswith('iPhone')) or (family=='tvOS' and t['name'].startswith('Apple TV')) or (family=='watchOS' and t['name'].startswith('Apple Watch')))]
            if not candidates:raise ValueError(f'no simulator device type for installed {family} runtime')
            device=run(['xcrun','simctl','create','Nova T19 '+uuid.uuid4().hex[:8],candidates[-1]['identifier'],runtime['identifier']],ROOT,output/(family+'-create.log')).strip()
            owned.append(device)
            run(['xcrun','simctl','boot',device],ROOT,output/(family+'-boot.log'))
            run(['xcrun','simctl','bootstatus',device,'-b'],ROOT,output/(family+'-bootstatus.log'))
            simulators[family.lower()]=(device,runtime['version'])
        for distribution,dependency in [('source',ROOT),('binary',bundle)]:
            consumer=output/distribution;src=consumer/'Sources/IntegrationSmoke';src.mkdir(parents=True)
            (src/'main.swift').write_text(source)
            (consumer/'Package.swift').write_text('// swift-tools-version: 6.0\nimport PackageDescription\nlet package = Package(name: "IntegrationSmoke", platforms: '+PLATFORMS+', products: [.executable(name: "IntegrationSmoke", targets: ["IntegrationSmoke"])], dependencies: [.package(name: "NovaGraph", path: '+json.dumps(str(dependency))+')], targets: [.executableTarget(name: "IntegrationSmoke", dependencies: [.product(name: "GraphDBKit", package: "NovaGraph")])])\n')
            for row in [s for s in SLICES if s['id'] in args.platforms]:
                logs=output/(distribution+'-'+row['id']);logs.mkdir()
                derived=logs/'DerivedData'
                print(f'{distribution}: building {row["id"]}',flush=True)
                run(['xcodebuild','-scheme','IntegrationSmoke','-configuration','Release','-destination','generic/platform='+DESTINATIONS[row['id']],'-derivedDataPath',derived,'CODE_SIGNING_ALLOWED=NO','ONLY_ACTIVE_ARCH=NO','ARCHS='+' '.join(row['architectures']),'build'],consumer,logs/'build.log')
                paths=[p for p in (derived/'Build/Products').glob('*/IntegrationSmoke') if p.is_file()]
                if len(paths)!=1:raise ValueError(f'expected one executable for {row["id"]}, found {paths}')
                executable=paths[0]
                archs=run(['xcrun','lipo','-archs',executable],ROOT,logs/'architectures.log').split()
                if sorted(archs)!=sorted(row['architectures']):raise ValueError('consumer missing required architecture')
                builds.append(dict(id=row['id'],distribution=distribution,architectures=archs,status='passed'))
                if row['id'] in ('macos','catalyst'):
                    version=subprocess.check_output(['sw_vers','-productVersion'],text=True).strip()
                    for arch in archs:
                        archlogs=logs/arch;archlogs.mkdir()
                        smoke(['arch','-'+arch,executable],ROOT,archlogs,archlogs/'database')
                        record(row,arch,distribution,'native',version)
                elif row['variant']=='simulator' and row['platform'] in simulators:
                    device,version=simulators[row['platform']]
                    # Modern arm64 hosts run their native simulator ABI. Other ABI runtimes
                    # remain explicitly unqualified, even though both architectures link.
                    arch=os.uname().machine;archlogs=logs/arch;archlogs.mkdir()
                    smoke(['xcrun','simctl','spawn','-a',arch,device,executable],ROOT,archlogs,archlogs/'database')
                    record(row,arch,distribution,'simulator',version)
                save()
            if args.device:
                logs=output/(distribution+'-device');logs.mkdir();(logs/'main.swift').write_text(app_source)
                identifier='org.novagraph.artifactsmoke.'+distribution
                project={'name':'NovaArtifactSmoke','packages':{'NovaGraph':{'path':str(dependency)}},'targets':{'NovaArtifactSmoke':{'type':'application','platform':'iOS','deploymentTarget':'16.0','sources':['main.swift'],'dependencies':[{'package':'NovaGraph','product':'GraphDBKit'}],'settings':{'base':{'PRODUCT_BUNDLE_IDENTIFIER':identifier,'DEVELOPMENT_TEAM':args.team,'CODE_SIGN_STYLE':'Automatic','GENERATE_INFOPLIST_FILE':'YES','SWIFT_VERSION':'6.0','TARGETED_DEVICE_FAMILY':'1','INFOPLIST_KEY_UILaunchScreen_Generation':'YES'}}}}}
                (logs/'project.json').write_text(json.dumps(project,indent=2))
                run(['xcodegen','generate','--spec','project.json'],logs,logs/'generate.log')
                run(['xcodebuild','-project','NovaArtifactSmoke.xcodeproj','-scheme','NovaArtifactSmoke','-configuration','Release','-destination','id='+args.device,'-derivedDataPath',logs/'DerivedData','-allowProvisioningUpdates','build'],logs,logs/'build.log')
                app=logs/'DerivedData/Build/Products/Release-iphoneos/NovaArtifactSmoke.app'
                run(['xcrun','devicectl','device','install','app','--device',args.device,app],ROOT,logs/'install.log')
                try:
                    smoke(['xcrun','devicectl','device','process','launch','--device',args.device,'--console','--terminate-existing',identifier],ROOT,logs)
                    details=json.loads(run(['xcrun','devicectl','device','info','details','--device',args.device,'--json-output','-'],ROOT,logs/'device.log'))
                    version=details['result']['deviceProperties']['osVersionNumber']
                    record(next(s for s in SLICES if s['id']=='ios'),'arm64',distribution,'device',version);save()
                finally:run(['xcrun','devicectl','device','uninstall','app','--device',args.device,identifier],ROOT,logs/'uninstall.log')
        save();print(f'Consumer evidence: {output}/evidence.json',flush=True)
    finally:
        save()
        for device in owned:
            subprocess.run(['xcrun','simctl','shutdown',device],capture_output=True)
            subprocess.run(['xcrun','simctl','delete',device],capture_output=True)

if __name__=='__main__':
    try:main()
    except (ValueError,subprocess.TimeoutExpired) as error:sys.exit(str(error))
