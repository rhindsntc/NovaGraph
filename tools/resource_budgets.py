#!/usr/bin/env python3
"""Validate sampled disk and complete Apple distribution size budgets."""
import argparse
import json
import importlib.util
import re
from pathlib import Path
import apple_artifacts as apple
import performance as p
import resource_measurements as resources
import linked_app_size as linked
spec=importlib.util.spec_from_file_location("swift_measurements",p.ROOT/"tools/swift-measurements.py")
swift=importlib.util.module_from_spec(spec);spec.loader.exec_module(swift)


def provenance(report, revision, source_digest):
    for value,length in ((revision,40),(source_digest,64)):
        if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{'+str(length)+'}',value):
            raise ValueError('expected candidate revision and source digest are required')
    if report.get('dirty') is not False or report.get('revision')!=revision or report.get('sourceDigest')!=source_digest:
        raise ValueError('dirty or foreign resource candidate')


def evaluate_disk(report, budgets, revision, source_digest):
    p.validate_budgets(budgets)
    provenance(report,revision,source_digest)
    summary=resources.summarize(report)
    if report['protocol'].get('version')!=2:raise ValueError('historical disk evidence cannot qualify current budgets')
    limits=budgets['classes'][report['profile']]
    failures=[];unavailable=[];rows=[]
    for work in summary['tables']:
        row={'name':work['name'],'metrics':{}}
        for observed,key in [('maxLogicalBytes','diskLogicalBytes'),('maxAllocatedBytes','diskAllocatedBytes'),('maxFiles','diskFiles')]:
            value=work[observed]
            row['metrics'][key]={'maximum':value,'limit':limits[key]}
            if value is None:
                unavailable.append(work['name']+' '+key)
                # Unknown snapshots do not erase breaches we actually observed.
                snapshots=next(w['runs'][0]['resources']['diskSnapshots'] for w in report['workloads'] if w['name']==work['name'])
                known=[s['allocatedBytes'] for s in snapshots if s['allocatedBytes'] is not None]
                row['metrics'][key]['maximumKnown']=max(known) if known else None
                if known and max(known)>limits[key]:failures.append(work['name']+' '+key+' exceeds budget')
            elif value>limits[key]:failures.append(work['name']+' '+key+' exceeds budget')
        rows.append(row)
    return {'status':'failed' if failures else 'unqualified' if unavailable else 'passed',
            'scope':'Maximum across seven sampled lifecycle boundaries; not transient peak or long-term growth',
            'hardwareQualified':False,'timingQualified':False,'budgetDigest':p.budget_digest(budgets),
            'revision':revision,'sourceDigest':source_digest,'failures':failures,'unavailable':unavailable,'tables':rows}


def evaluate_distribution(bundle, archive, budgets, revision, source_digest):
    p.validate_budgets(budgets)
    # Full ABI/platform validation precedes the size/hash/ZIP revalidation boundary.
    manifest=apple.verify_bundle(bundle)
    provenance(manifest,revision,source_digest)
    sizes=apple.resource_size_report(bundle,manifest,archive)
    failures=[]
    for key in ('archiveBytes','bundleLogicalBytes'):
        if sizes[key]>budgets['distribution'][key]:failures.append(key+' exceeds budget')
    return {'status':'failed' if failures else 'passed','hardwareQualified':False,
            'scope':budgets['distribution']['scope'],'budgetDigest':p.budget_digest(budgets),
            'revision':revision,'sourceDigest':source_digest,'failures':failures,'sizes':sizes,
            'limits':budgets['distribution']}


def evaluate_linked(report_path, budgets, revision, source_digest):
    p.validate_budgets(budgets)
    if budgets.get('linkedApp',{}).get('protocolDigest')!=linked.PROTOCOL_DIGEST: raise ValueError('approved current linked-app fixture caps are required')
    report=linked.verify(report_path,revision,source_digest)
    limit=budgets['linkedApp']['bundleLogicalBytes']
    failures=[row['platform']+' linked-app contribution exceeds budget' for row in report['deltas'] if row['bundleLogicalBytes']>limit]
    return {'status':'failed' if failures else 'passed','hardwareQualified':False,
            'scope':budgets['linkedApp']['scope'],'budgetDigest':p.budget_digest(budgets),
            'revision':revision,'sourceDigest':source_digest,'failures':failures,
            'tables':[{**row,'limit':limit} for row in report['deltas']]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision',required=True)
    parser.add_argument('--source-digest',required=True)
    parser.add_argument('--output',type=Path,required=True)
    sub=parser.add_subparsers(dest='kind',required=True)
    disk=sub.add_parser('disk');disk.add_argument('report',type=Path)
    distribution=sub.add_parser('distribution');distribution.add_argument('--bundle',type=Path,required=True);distribution.add_argument('--archive',type=Path,required=True)
    swift_parser=sub.add_parser('swift');swift_parser.add_argument('report',type=Path)
    linked_parser=sub.add_parser('linked-app');linked_parser.add_argument('report',type=Path)
    args=parser.parse_args()
    if args.output.exists():raise ValueError('output must be new; evidence is never overwritten')
    budgets=json.loads((p.ROOT/'benchmarks/budgets.json').read_text())
    if args.kind=='disk':result=evaluate_disk(json.loads(args.report.read_text()),budgets,args.revision,args.source_digest)
    elif args.kind=='distribution':result=evaluate_distribution(args.bundle,args.archive,budgets,args.revision,args.source_digest)
    elif args.kind=='swift':result=swift.evaluate_budget(json.loads(args.report.read_text()),budgets,args.revision,args.source_digest)
    else:result=evaluate_linked(args.report,budgets,args.revision,args.source_digest)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x') as output:output.write(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    return 0 if result['status']=='passed' else 1

if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,KeyError,OSError) as error:raise SystemExit(str(error))
