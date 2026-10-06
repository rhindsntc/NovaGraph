#!/usr/bin/env python3
"""T21: repeatable native measurements, explicit budgets and retained evidence."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import uuid
from performance_lock import measurement_lock
from resource_measurements import summarize as summarize_resources, validate_mutation_protocol
ROOT=Path(__file__).resolve().parents[1]
WORKLOADS=('contacts','messages','knowledge','navigation','recommendations')
METRICS=('hotReadUs','coldReadUs','checkpointUs','openUs','trimUs','novaTraversalUs','singleMutationUs','batchMutationUs')
SAMPLED_METRICS=('hotReadUs','coldReadUs','novaTraversalUs','singleMutationUs','batchMutationUs')

def validate_budgets(budgets):
    if not isinstance(budgets,dict) or type(budgets.get('schemaVersion')) is not int or budgets['schemaVersion']!=2 or budgets.get('protocolVersion')!=2:
        raise ValueError('unsupported budget schema/protocol')
    review=budgets.get('review',{})
    if review.get('status')!='approved' or not re.fullmatch(r'\d{4}-\d{2}-\d{2}',review.get('date','')) or not review.get('source'):
        raise ValueError('budgets require explicit approval metadata')
    if budgets.get('units')!={'timing':'microseconds','memory':'bytes','disk':'bytes','files':'count','distribution':'bytes'}:
        raise ValueError('budget units must be explicit')
    def positive(value):
        if type(value) not in (int,float) or not math.isfinite(value) or value<=0:raise ValueError('budget limits must be finite positive numbers')
    for key,minimum in [('minimumRuns',20),('minimumSamples',100)]:
        if type(budgets.get(key)) is not int or budgets[key]<minimum:raise ValueError('insufficient budget sampling policy')
    positive(budgets.get('regressionFraction'))
    if budgets['regressionFraction']!=0.1:raise ValueError('unsupported regression policy')
    if set(budgets.get('classes',{}))!={'watch','phone','desktop'}:raise ValueError('missing budget classes')
    for profile,nodes in [('watch',250),('phone',1000),('desktop',10000)]:
        limits=budgets['classes'][profile]
        if type(limits.get('nodes')) is not int or limits['nodes']!=nodes:raise ValueError('unexpected workload scale')
        for key in (*METRICS,'peakRSSBytes','diskLogicalBytes','diskAllocatedBytes','diskFiles'):positive(limits.get(key))
        for key in ('peakRSSBytes','diskLogicalBytes','diskAllocatedBytes','diskFiles'):
            if type(limits[key]) is not int:raise ValueError('resource limits must be integer counts')
    for key in ('archiveBytes','bundleLogicalBytes'):
        positive(budgets.get('distribution',{}).get(key))
        if type(budgets['distribution'][key]) is not int:raise ValueError('distribution limits must be integer bytes')
    # Historical native budget files need not contain these independently reviewed scopes.
    for section in ('swift','linkedApp'):
        if section not in budgets: continue
        limits=budgets[section]
        review=limits.get('review',{})
        if type(limits.get('version')) is not int or limits['version']!=1 or review.get('status')!='approved' or not re.fullmatch(r'\d{4}-\d{2}-\d{2}',review.get('date','')) or not review.get('source') or not limits.get('scope'):
            raise ValueError(section+' limits require explicit approval and scope')
        if section=='swift':
            lanes={'swiftTypedUs','swiftColdUs','swiftSingleMutationUs','swiftBatch250Us','swiftTraversalUs','swiftAsyncReadUs','swiftAsyncTraversalUs','swiftAsyncMutationUs'}
            if limits.get('platform')!='macos' or limits.get('payloadBytes')!=[64,512,1024] or any(type(x) is not int for x in limits['payloadBytes']) or not re.fullmatch('[0-9a-f]{64}',limits.get('protocolDigest','')) or set(limits.get('metrics',{}))!=lanes:
                raise ValueError('unsupported Swift budget scope')
            for value in limits['metrics'].values(): positive(value)
        else:
            if limits.get('platforms')!=['macos','ios'] or not re.fullmatch('[0-9a-f]{64}',limits.get('protocolDigest','')): raise ValueError('unsupported linked-app budget scope')
            positive(limits.get('bundleLogicalBytes'))
            if type(limits['bundleLogicalBytes']) is not int: raise ValueError('linked-app limit must be integer bytes')
    return budgets

def budget_digest(budgets):
    validate_budgets(budgets)
    return hashlib.sha256(json.dumps(budgets,sort_keys=True,separators=(',',':'),allow_nan=False).encode()).hexdigest()

def measurement_protocol(budgets,profile,mode):
    validate_budgets(budgets)
    if profile not in budgets['classes'] or mode not in ('smoke','measure','resources'):raise ValueError('unknown measurement profile/mode')
    smoke=mode=='smoke'
    return {'version':2,'mutationBatchSize':250,'seed':42,'nodes':32 if smoke else budgets['classes'][profile]['nodes'],
            'runs':2 if smoke else 1 if mode=='resources' else budgets['minimumRuns'],
            'samples':8 if smoke else budgets['minimumSamples'],'warmup':2 if smoke else 20,'mode':mode}


def percentile(samples,fraction):
    if not samples or not 0<fraction<=1 or any(type(x) not in (int,float) or not math.isfinite(x) or x<0 for x in samples):
        raise ValueError('samples must be nonempty, finite and nonnegative')
    return sorted(samples)[math.ceil(len(samples)*fraction)-1]

def evaluate(report,budgets,baseline=None):
    validate_budgets(budgets)
    if report.get('kind')=='nova-activity-diagnostic':raise ValueError('activity diagnostic cannot qualify timing budgets')
    if report.get('protocol',{}).get('mode')=='resources' or any('resources' in run for work in report.get('workloads',[]) for run in work.get('runs',[])):
        raise ValueError('instrumented resource measurements cannot qualify timing budgets')
    if report.get('complete') is not True:raise ValueError('incomplete measurement cannot qualify')
    if report.get('schemaVersion')!=1:raise ValueError('unsupported measurement schema')
    if type(report.get('dirty')) is not bool:raise ValueError('explicit source cleanliness is required')
    for field,length in (('revision',40),('sourceDigest',64)):
        if not isinstance(report.get(field),str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}',report[field]):raise ValueError('missing or invalid '+field)
    if not isinstance(report.get('runFailures'),list) or report['runFailures']:raise ValueError('missing or nonempty failure inventory')
    profile=report['profile'];limits=budgets['classes'][profile];protocol=report['protocol'];env=report['environment']
    if protocol.get('version')!=2 or type(protocol.get('version')) is not int or protocol.get('mutationBatchSize')!=250:raise ValueError('unsupported measurement protocol')
    for key in ('nodes','runs','samples','warmup','seed','mutationBatchSize'):
        if type(protocol.get(key)) is not int or protocol[key]<=0:raise ValueError('invalid protocol inventory')
    if protocol['seed']!=42:raise ValueError('unexpected workload seed')
    smoke=protocol['mode']=='smoke'
    if protocol['mode'] not in ('smoke','measure'):raise ValueError('unknown protocol mode')
    if not smoke and (protocol['nodes']!=limits['nodes'] or protocol['runs']<budgets['minimumRuns'] or protocol['samples']<budgets['minimumSamples'] or protocol['warmup']<20):raise ValueError('wrong workload scale or insufficient independent samples/warmup')
    if sorted(w['name'] for w in report['workloads'])!=sorted(WORKLOADS):raise ValueError('missing or duplicate workload')
    if baseline is not None:
        if baseline.get('dirty',False):raise ValueError('dirty baseline cannot qualify a regression')
        if baseline['environment']!=env or baseline['protocol']!=protocol or baseline['profile']!=profile:raise ValueError('incomparable baseline environment or protocol')
        previous=evaluate(baseline,budgets)
        if previous['status']!='passed' or not previous['hardwareQualified']:raise ValueError('baseline fails absolute limits or qualification')
    failures=[];tables=[]
    for work in report['workloads']:
        runs=work['runs']
        if len(runs)!=protocol['runs']:raise ValueError('run count differs from protocol')
        for run in runs:
            validate_mutation_protocol(run,protocol['samples'],protocol['warmup'])
            dataset=run.get('dataset',{})
            expected={'name':work['name'],'nodes':protocol['nodes'],'seed':protocol['seed'],'nodeStringPayloadBytes':1024 if work['name']=='knowledge' else 512 if work['name']=='messages' else 64,'nodePropertyCount':2,'edgePropertyCount':0}
            if run.get('platform')!=env['platform'] or any(dataset.get(k)!=v or type(dataset.get(k)) is not type(v) for k,v in expected.items()):raise ValueError('run platform or dataset differs from protocol')
            if type(dataset.get('edges')) is not int or dataset['edges']<=0:raise ValueError('invalid dataset edge count')
            if dataset!=runs[0]['dataset']:raise ValueError('inconsistent dataset shape between runs')
            if baseline is not None:
                old=next(w for w in baseline['workloads'] if w['name']==work['name'])
                if dataset!=old['runs'][0]['dataset']:raise ValueError('incomparable baseline dataset shape')
            for metric in METRICS:
                values=run.get('metrics',{}).get(metric)
                if not isinstance(values,list):raise ValueError('missing metric samples')
                percentile(values,0.95)
                count=protocol['samples'] if metric in SAMPLED_METRICS else 1
                if len(values)!=count:raise ValueError('metric sample inventory mismatch')
            percentile([run['peakRSSBytes']],1)
        row={'name':work['name'],'metrics':{}}
        for metric in METRICS:
            values=[v for run in runs for v in run['metrics'][metric]]
            p50=percentile(values,0.5);p95=percentile(values,0.95)
            row['metrics'][metric]={'p50':p50,'p95':p95,'p99':percentile(values,0.99),'samples':len(values),'limit':limits[metric]}
            if p95>limits[metric]:failures.append(f"{work['name']} {metric} p95 {p95:.3f} > {limits[metric]}")
            if baseline is not None:
                old=next(w for w in baseline['workloads'] if w['name']==work['name'])
                # Compare both center and tail across independent processes. For
                # singleton lifecycle metrics these are the process median and p95.
                for within in (0.5,0.95):
                    current=[percentile(r['metrics'][metric],within) for r in runs]
                    previous=[percentile(r['metrics'][metric],within) for r in old['runs']]
                    for across in (0.5,0.95):
                        now=statistics.median(current) if across==0.5 else percentile(current,across)
                        before=statistics.median(previous) if across==0.5 else percentile(previous,across)
                        if now>before*(1+budgets['regressionFraction']):failures.append(f"{work['name']} {metric} process p{int(across*100)} of operation p{int(within*100)} regression >10%")
        row['peakRSSBytes']=max(r['peakRSSBytes'] for r in runs)
        if row['peakRSSBytes']>limits['peakRSSBytes']:failures.append(f"{work['name']} peak RSS exceeds budget")
        tables.append(row)
    qualified=not smoke and not report.get('dirty',False) and env['platform']=={'desktop':'macos','phone':'ios-device','watch':'watchos-device'}[profile]
    return {'status':'unqualified' if smoke else 'failed' if failures else 'passed' if qualified else 'unqualified','hardwareQualified':qualified and not failures,'failures':failures,'tables':tables}

def command(args,**kwargs):
    return subprocess.check_output([str(a) for a in args],text=True,**kwargs).strip()

def source_digest():
    paths=[]
    for folder in ('cpp/src','cpp/include','cpp/c_api','cpp/third_party'):
        paths.extend(p for p in (ROOT/folder).rglob('*') if p.is_file())
    # Private plans and collected reports do not affect the benchmark executable.
    benchmark_suffixes={'.c','.cc','.cpp','.cxx','.h','.hh','.hpp','.hxx','.inc'}
    paths.extend(p for p in (ROOT/'benchmarks/performance').rglob('*') if p.is_file() and p.suffix.lower() in benchmark_suffixes)
    paths.extend([Path(__file__), ROOT/'tools/resource_measurements.py', ROOT/'tools/performance_lock.py', ROOT/'benchmarks/budgets.json'])
    digest=hashlib.sha256()
    for path in sorted(paths):digest.update(str(path.relative_to(ROOT)).encode()+b'\0'+path.read_bytes()+b'\0')
    return digest.hexdigest()

def build(output,simulator,resources=False):
    sdk='iphonesimulator' if simulator else 'macosx';sdkpath=command(['xcrun','--sdk',sdk,'--show-sdk-path'])
    target=platform.machine()+('-apple-ios16.0-simulator' if simulator else '-apple-macos13.0')
    binary=output/'nova-performance'
    args=['xcrun','clang++','-std=c++20','-O3','-DNDEBUG','-target',target,'-isysroot',sdkpath,'-I',ROOT/'cpp/include','-I',ROOT/'cpp/third_party',ROOT/'benchmarks/performance/native.cpp',*sorted((ROOT/'cpp/src').glob('*.cpp')),'-lsqlite3','-pthread','-o',binary]
    if resources:args.insert(2,'-DNOVA_RESOURCE_DIAGNOSTICS')
    with (output/'build.log').open('w') as log:subprocess.run([str(a) for a in args],check=True,stdout=log,stderr=subprocess.STDOUT,timeout=600)
    return binary

def collect_workloads(prefix,report,out,timeout=600):
    protocol=report['protocol'];platform_name=report['environment']['platform']
    def output_text(value):
        return value.decode('utf-8',errors='replace') if isinstance(value,bytes) else value or ''
    for workload in WORKLOADS:
        item={'name':workload,'runs':[]};report['workloads'].append(item)
        for i in range(protocol['runs']):
            print(f"{platform_name} {workload} {i+1}/{protocol['runs']}",flush=True)
            stdout='';stderr='';failure=None
            try:
                proc=subprocess.run([*prefix,workload,str(protocol['nodes']),str(protocol['seed']),str(protocol['samples']),str(protocol['warmup'])],text=True,capture_output=True,timeout=timeout)
                stdout=proc.stdout;stderr=proc.stderr
                if proc.returncode:failure={'kind':'exit','exitCode':proc.returncode,'error':stderr[-1000:]}
            except subprocess.TimeoutExpired as error:
                stdout=output_text(error.stdout);stderr=output_text(error.stderr)
                failure={'kind':'timeout','exitCode':None,'error':f'timed out after {timeout} seconds'}
            except OSError as error:
                failure={'kind':'launch','exitCode':None,'error':str(error)}
            (out/f'{workload}-{i}.stdout').write_text(stdout)
            (out/f'{workload}-{i}.stderr').write_text(stderr)
            if failure is None:
                try:
                    result=json.loads(stdout)
                    if result['platform']!=platform_name or result['dataset']['nodes']!=protocol['nodes']:raise ValueError('binary platform/workload mismatch')
                except (ValueError,KeyError,TypeError) as error:
                    failure={'kind':'invalid-output','exitCode':0,'error':str(error)}
            if failure is not None:
                report['complete']=False
                report['runFailures'].append({'workload':workload,'run':i,**failure})
                break
            item['runs'].append(result)

def run(args):
    # Ordinary smoke/resource runs must not build or run beside controlled timing.
    with measurement_lock():
        return _run_locked(args)

def _run_locked(args):
    resources=args.mode=='resources'
    if resources and args.baseline:raise ValueError('resource diagnostics cannot use a baseline')
    budgets=json.loads((ROOT/'benchmarks/budgets.json').read_text())
    protocol=measurement_protocol(budgets,args.profile,args.mode)
    out=args.output.resolve()
    if out.exists():raise ValueError('output must be new; old evidence is never overwritten')
    out.mkdir(parents=True)
    sim=args.platform=='ios-simulator';owned=None
    report={'schemaVersion':1,'revision':command(['git','rev-parse','HEAD'],cwd=ROOT),'dirty':bool(command(['git','status','--porcelain'],cwd=ROOT)),'sourceDigest':source_digest(),'profile':args.profile,'environment':{'platform':args.platform,'hardware':command(['sysctl','-n','hw.model']),'os':platform.platform(),'toolchain':command(['xcodebuild','-version']),'architecture':platform.machine()},'protocol':protocol,'workloads':[],'complete':False,'runFailures':[]}
    try:
        binary=build(out,sim,resources=True) if resources else build(out,sim);prefix=[]
        if sim:
            runtimes=json.loads(command(['xcrun','simctl','list','runtimes','-j']))['runtimes']
            runtime=max((r for r in runtimes if r.get('isAvailable') and r['name'].startswith('iOS ')),key=lambda r:tuple(map(int,r['version'].split('.'))))
            device=next(t for t in runtime['supportedDeviceTypes'] if t['name'].startswith('iPhone'))
            owned=command(['xcrun','simctl','create','Nova performance '+uuid.uuid4().hex[:8],device['identifier'],runtime['identifier']])
            command(['xcrun','simctl','boot',owned]);command(['xcrun','simctl','bootstatus',owned,'-b'])
            prefix=['xcrun','simctl','spawn',owned];report['environment']['simulator']={'runtime':runtime['version'],'device':device['name']}
        collect_workloads([*prefix,str(binary)],report,out)
        if source_digest()!=report['sourceDigest'] or command(['git','rev-parse','HEAD'],cwd=ROOT)!=report['revision']:
            report['runFailures'].append({'kind':'source-changed','error':'source or revision changed during collection'})
        report['complete']=not report['runFailures']
        if report['complete']:
            if resources:report['evaluation']=summarize_resources(report)
            else:
                baseline=json.loads(args.baseline.read_text()) if args.baseline else None
                report['evaluation']=evaluate(report,budgets,baseline)
    except (ValueError,KeyError,StopIteration,OSError,subprocess.SubprocessError) as error:
        report['complete']=False
        report['runFailures'].append({'kind':'collection','error':str(error)})
    finally:
        if not report['complete']:
            report['evaluation']={'status':'failed','hardwareQualified':False,'failures':report['runFailures'],'tables':[]}
        (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        if owned:
            subprocess.run(['xcrun','simctl','shutdown',owned],capture_output=True)
            subprocess.run(['xcrun','simctl','delete',owned],capture_output=True)
    print(json.dumps(report['evaluation'],indent=2))
    return 1 if report['evaluation']['status']=='failed' else 0

def main():
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='action',required=True)
    r=sub.add_parser('run');r.add_argument('--output',type=Path,required=True);r.add_argument('--platform',choices=['macos','ios-simulator'],default='macos');r.add_argument('--profile',choices=['watch','phone','desktop'],default='desktop');r.add_argument('--mode',choices=['smoke','measure','resources'],default='smoke');r.add_argument('--baseline',type=Path)
    e=sub.add_parser('evaluate');e.add_argument('report',type=Path);e.add_argument('--baseline',type=Path)
    args=parser.parse_args()
    if args.action=='run':return run(args)
    result=evaluate(json.loads(args.report.read_text()),json.loads((ROOT/'benchmarks/budgets.json').read_text()),json.loads(args.baseline.read_text()) if args.baseline else None);print(json.dumps(result,indent=2));return 0 if result['status']=='passed' else 1
if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,KeyError,StopIteration,OSError,subprocess.SubprocessError) as error:print(f'Performance validation failed: {error}',file=sys.stderr);sys.exit(1)
