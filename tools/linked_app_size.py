#!/usr/bin/env python3
"""Measure matched minimal SwiftUI apps, preserving builds and rechecking bundle bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import apple_artifacts as apple
import performance as p

ROOT=p.ROOT
FIXTURE=ROOT/'examples/LinkedAppSize/App.swift'
PLATFORMS={'macos':('macosx','generic/platform=macOS','13.0',1),
           'ios':('iphoneos','generic/platform=iOS','16.0',2)}
SETTINGS={'PRODUCT_BUNDLE_IDENTIFIER':'org.novagraph.sizeprobe','PRODUCT_NAME':'Footprint',
          'SWIFT_VERSION':'6.0','GENERATE_INFOPLIST_FILE':'YES','CODE_SIGNING_ALLOWED':'NO',
          'DEAD_CODE_STRIPPING':'YES','STRIP_INSTALLED_PRODUCT':'YES','DEPLOYMENT_POSTPROCESSING':'YES',
          'DEBUG_INFORMATION_FORMAT':'dwarf','GCC_GENERATE_DEBUGGING_SYMBOLS':'NO'}
PROTOCOL={'version':1,'platforms':['macos','ios'],'variants':['baseline','nova'],
          'architecture':'arm64','configuration':'Release','settings':SETTINGS,
          'minimumOS':{name:row[2] for name,row in PLATFORMS.items()},
          'fixtureSHA256':hashlib.sha256(FIXTURE.read_bytes()).hexdigest()}

def canonical(value): return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)
PROTOCOL_DIGEST=hashlib.sha256(canonical(PROTOCOL).encode()).hexdigest()

def source_digest():
    paths=[ROOT/'Package.swift',FIXTURE,Path(__file__),ROOT/'tools/apple_artifacts.py',ROOT/'tools/performance.py']
    for folder in ('cpp/src','cpp/include','cpp/c_api','cpp/third_party','swift/Sources'):
        paths.extend(path for path in (ROOT/folder).rglob('*') if path.is_file())
    digest=hashlib.sha256()
    for path in sorted(set(paths)): digest.update(str(path.relative_to(ROOT)).encode()+b'\0'+path.read_bytes()+b'\0')
    return digest.hexdigest()

def project_spec(platform,variant):
    settings=dict(SETTINGS)
    if variant=='nova': settings['SWIFT_ACTIVE_COMPILATION_CONDITIONS']='$(inherited) WITH_NOVA'
    target={'type':'application','platform':'macOS' if platform=='macos' else 'iOS',
            'deploymentTarget':PLATFORMS[platform][2],'sources':['App.swift'],'settings':{'base':settings}}
    spec={'name':'Footprint','targets':{'Footprint':target},'schemes':{'Footprint':{'build':{'targets':{'Footprint':'all'}}}}}
    if variant=='nova':
        spec['packages']={'Nova':{'path':str(ROOT)}}
        target['dependencies']=[{'package':'Nova','product':'GraphDBKit'}]
    return spec

def build_command(directory,platform):
    sdk,destination,_,_=PLATFORMS[platform]
    return ['xcodebuild','-project','Footprint.xcodeproj','-scheme','Footprint','-configuration','Release',
            '-sdk',sdk,'-destination',destination,'ARCHS=arm64','-derivedDataPath',str(directory/'DerivedData'),
            'CODE_SIGNING_ALLOWED=NO','build']

def product_path(platform):
    return Path('DerivedData/Build/Products')/('Release' if platform=='macos' else 'Release-iphoneos')/'Footprint.app'

def bundle_files(bundle):
    if bundle.is_symlink() or not bundle.is_dir(): raise ValueError('missing or linked app bundle')
    files=[]
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink(): raise ValueError('symlink in measured app bundle')
        if path.is_file(): files.append({'path':str(path.relative_to(bundle)),'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    if not files: raise ValueError('empty app bundle')
    return files

def build_metadata(directory,platform,variant):
    project=directory/'Footprint.xcodeproj/project.pbxproj'
    log=directory/'build.log'
    if any(path.is_symlink() for path in (project.parent,project,log)): raise ValueError('linked build metadata')
    text=project.read_text()
    if 'App.swift in Sources' not in text or ('GraphDBKit in Frameworks' in text)!=(variant=='nova'):
        raise ValueError('generated project has wrong source or package dependency')
    # Resolve settings from the actual consumed project, not just its generator input.
    output=json.loads(p.command([*build_command(directory,platform)[:-1],'-showBuildSettings','-json'],cwd=directory))
    targets=[row['buildSettings'] for row in output if row.get('target')=='Footprint']
    if len(targets)!=1: raise ValueError('missing effective linked-app target settings')
    expected={**SETTINGS,'CONFIGURATION':'Release','ARCHS':'arm64','PLATFORM_NAME':PLATFORMS[platform][0]}
    settings=targets[0]
    if any(settings.get(key)!=value for key,value in expected.items()): raise ValueError('mismatched effective linked-app settings')
    flags=settings.get('SWIFT_ACTIVE_COMPILATION_CONDITIONS','').split()
    if ('WITH_NOVA' in flags)!=(variant=='nova'): raise ValueError('wrong linked-app compilation variant')
    log_text=log.read_text()
    if not re.search(r'^Strip .*Footprint',log_text,re.MULTILINE) or re.search(r'^CodeSign ',log_text,re.MULTILINE) or '** BUILD SUCCEEDED **' not in log_text:
        raise ValueError('missing successful stripped unsigned app build')
    return {'projectSHA256':hashlib.sha256(project.read_bytes()).hexdigest(),
            'buildLogSHA256':hashlib.sha256(log.read_bytes()).hexdigest(),
            'effectiveSettings':{key:settings[key] for key in expected}|{'SWIFT_ACTIVE_COMPILATION_CONDITIONS':flags}}

def inspect(directory,platform,variant):
    bundle=directory/product_path(platform)
    binary=bundle/('Contents/MacOS/Footprint' if platform=='macos' else 'Footprint')
    files=bundle_files(bundle)
    metadata=build_metadata(directory,platform,variant)
    if p.command(['xcrun','lipo','-archs',binary])!='arm64': raise ValueError('unexpected linked-app architecture')
    load_commands=p.command(['xcrun','otool','-l',binary])
    apple.validate_load_commands(load_commands,PLATFORMS[platform][3],PLATFORMS[platform][2],1)
    return {'platform':platform,'variant':variant,'app':str(product_path(platform)),
            'bundleLogicalBytes':sum(row['bytes'] for row in files),'bundleFiles':len(files),
            'executableBytes':binary.stat().st_size,'files':files,'loadCommandsSHA256':hashlib.sha256(load_commands.encode()).hexdigest(),
            'command':build_command(directory,platform),'buildMetadata':metadata,
            'smoke':p.command([binary,'--verify']) if platform=='macos' else 'not run; unsigned device build only'}

def verify(report_path,revision,source_digest):
    for value,length in ((revision,40),(source_digest,64)):
        if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}',value): raise ValueError('expected candidate provenance is required')
    if report_path.is_symlink(): raise ValueError('linked report cannot be a symlink')
    out=report_path.resolve().parent
    report=json.loads(report_path.read_text())
    if report.get('dirty') is not False or report.get('revision')!=revision or report.get('sourceDigest')!=source_digest:
        raise ValueError('dirty or foreign linked-app candidate')
    if type(report.get('schemaVersion')) is not int or report['schemaVersion']!=1 or report.get('kind')!='linked-app-size' or report.get('complete') is not True or report.get('failures')!=[]:
        raise ValueError('incomplete or historical linked-app report')
    if canonical(report.get('protocol'))!=canonical(PROTOCOL) or report.get('protocolDigest')!=PROTOCOL_DIGEST:
        raise ValueError('mismatched linked-app fixture or build settings')
    env=report.get('environment',{})
    if not isinstance(env.get('xcode'),str) or not env['xcode'] or set(env.get('sdks',{}))!=set(PLATFORMS) or any(not isinstance(v,str) or not v for v in env['sdks'].values()): raise ValueError('missing linked-app toolchain/SDK identity')
    rows=report.get('results',[])
    if not isinstance(rows,list) or len(rows)!=4: raise ValueError('missing matched app pair')
    expected=[(platform,variant) for platform in PLATFORMS for variant in ('baseline','nova')]
    for row,(platform,variant) in zip(rows,expected):
        directory=out/(platform+'-'+variant)
        if directory.is_symlink() or not directory.is_dir(): raise ValueError('missing or linked build directory')
        # Check the actual source/spec and bundle inventory, not saved deltas.
        if (directory/'App.swift').is_symlink() or (directory/'App.swift').read_bytes()!=FIXTURE.read_bytes(): raise ValueError('mismatched matched-app source')
        if (directory/'project.json').is_symlink() or canonical(json.loads((directory/'project.json').read_text()))!=canonical(project_spec(platform,variant)): raise ValueError('mismatched matched-app project')
        product=directory/product_path(platform)
        if any(path.is_symlink() for path in [product,*product.parents] if path!=out and out in path.parents): raise ValueError('app path escapes measured build')
        measured=inspect(directory,platform,variant)
        if canonical(row)!=canonical(measured) or measured['smoke']!=('verified-'+variant if platform=='macos' else 'not run; unsigned device build only'): raise ValueError('changed linked-app bytes or metadata')
    deltas=[{'platform':platform,'bundleLogicalBytes':rows[i+1]['bundleLogicalBytes']-rows[i]['bundleLogicalBytes'],
             'executableBytes':rows[i+1]['executableBytes']-rows[i]['executableBytes']} for i,platform in ((0,'macos'),(2,'ios'))]
    if any(row['bundleLogicalBytes']<0 or row['executableBytes']<0 for row in deltas): raise ValueError('negative linked-app contribution')
    return {**report,'deltas':deltas}

def logged(command,directory,log):
    with log.open('x') as stream: subprocess.run(command,cwd=directory,stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=1200)

def collect(out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=False)
    report={'schemaVersion':1,'kind':'linked-app-size','revision':p.command(['git','rev-parse','HEAD'],cwd=ROOT),
            'sourceDigest':source_digest(),'dirty':bool(p.command(['git','status','--porcelain'],cwd=ROOT)),
            'protocol':PROTOCOL,'protocolDigest':PROTOCOL_DIGEST,
            'environment':{'xcode':p.command(['xcodebuild','-version']),'sdks':{name:p.command(['xcrun','--sdk',row[0],'--show-sdk-version']) for name,row in PLATFORMS.items()}},
            'complete':False,'failures':[],'results':[]}
    try:
        if report['dirty']: raise ValueError('linked-app collection requires a clean checkout')
        for platform in PLATFORMS:
            for variant in ('baseline','nova'):
                directory=out/(platform+'-'+variant);directory.mkdir()
                (directory/'App.swift').write_bytes(FIXTURE.read_bytes())
                (directory/'project.json').write_text(json.dumps(project_spec(platform,variant),indent=2)+'\n')
                print('Building '+platform+' '+variant,flush=True)
                logged(['xcodegen','generate','--spec','project.json'],directory,directory/'generate.log')
                logged(build_command(directory,platform),directory,directory/'build.log')
                report['results'].append(inspect(directory,platform,variant))
        report['dirty']=bool(p.command(['git','status','--porcelain'],cwd=ROOT))
        if report['dirty'] or report['revision']!=p.command(['git','rev-parse','HEAD'],cwd=ROOT) or report['sourceDigest']!=source_digest(): raise ValueError('sources changed during linked-app collection')
        report['complete']=True
    except (ValueError,OSError,subprocess.SubprocessError) as error: report['failures'].append(str(error))
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['complete']: raise ValueError('linked-app measurement failed; see '+str(out/'results.json'))
    return verify(out/'results.json',report['revision'],report['sourceDigest'])

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    result=collect(parser.parse_args().output)
    print(json.dumps({'revision':result['revision'],'sourceDigest':result['sourceDigest'],'deltas':result['deltas']},indent=2))
if __name__=='__main__':
    try: main()
    except (ValueError,OSError,subprocess.SubprocessError) as error: raise SystemExit(str(error))
