#!/usr/bin/env python3
"""Build/run NovaGraph's real sample app; never installs on physical devices."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'examples/NovaGraphApp'

def validate_results(summary, tree, expected):
    if (summary.get('result') != 'Passed' or summary.get('passedTests') != len(expected)
        or summary.get('totalTestCount') != len(expected)
        or any(summary.get(key) != 0 for key in ('failedTests','skippedTests','expectedFailures'))):
        raise ValueError('UI suite failed, skipped cases, or ran an unexpected number of tests')
    cases=[]
    def visit(nodes):
        for node in nodes:
            if node.get('nodeType')=='Test Case':
                if node.get('result')!='Passed':raise ValueError('UI case did not pass')
                cases.append(node['nodeIdentifier'])
            visit(node.get('children',[]))
    visit(tree.get('testNodes',[]))
    if len(cases)!=len(expected) or set(cases)!=expected:raise ValueError('UI case inventory differs from expected cases')
    return sorted(cases)

def run(command, log, cwd=ROOT, timeout=900):
    with log.open('w') as stream:
        result=subprocess.run([str(arg) for arg in command],cwd=cwd,stdout=stream,stderr=subprocess.STDOUT,timeout=timeout)
    if result.returncode:raise ValueError(f'command failed; see {log}')
    return log.read_text()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--platforms',nargs='+',choices=['macos','ios'],default=['macos','ios'])
    args=parser.parse_args();out=args.output.resolve()
    if out.exists():parser.error('output must be a new directory; choose a new path for each acceptance run')
    out.mkdir(parents=True);owned=None
    report={'schemaVersion':1,'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()),
        'xcode':subprocess.check_output(['xcodebuild','-version'],text=True).strip(),
        'physicalDevices':'deferred; never installed by this harness','platforms':[],'passed':False}
    catalog=json.loads((ROOT/'tests/catalog.json').read_text())['tests']
    expected={t['id'].removeprefix('sample.').replace('.', '/', 1)+'()' for t in catalog if t['language']=='sample' and t['suite']=='sample-ui'}
    if not expected:raise ValueError('no sample UI cases in test catalog')
    try:
        run(['xcodegen','generate','--spec',APP/'project.json'],out/'generate.log')
        for platform in args.platforms:
            logs=out/platform;logs.mkdir()
            if platform=='macos':
                destination='platform=macOS,arch='+os.uname().machine;scheme='NovaGraphApp_macOS';extra=[]
            else:
                runtimes=json.loads(run(['xcrun','simctl','list','runtimes','-j'],logs/'runtimes.log'))['runtimes']
                available=[r for r in runtimes if r.get('isAvailable') and r['name'].startswith('iOS ')]
                if not available:raise ValueError('no installed iOS simulator runtime; qualification remains unavailable')
                runtime=max(available,key=lambda r:tuple(map(int,r['version'].split('.'))))
                types=[t for t in runtime['supportedDeviceTypes'] if t['name'].startswith('iPhone')]
                if not types:raise ValueError('installed iOS runtime has no iPhone simulator type')
                owned=run(['xcrun','simctl','create','Nova sample '+uuid.uuid4().hex[:8],types[0]['identifier'],runtime['identifier']],logs/'create.log').strip()
                run(['xcrun','simctl','boot',owned],logs/'boot.log')
                run(['xcrun','simctl','bootstatus',owned,'-b'],logs/'bootstatus.log')
                destination='platform=iOS Simulator,id='+owned;scheme='NovaGraphApp_iOS';extra=['CODE_SIGNING_ALLOWED=NO']
            result=logs/'tests.xcresult'
            print(f'{platform}: building and running real app UI tests',flush=True)
            run(['xcodebuild','-project',APP/'NovaGraphApp.xcodeproj','-scheme',scheme,'-destination',destination,'-derivedDataPath',logs/'DerivedData','-resultBundlePath',result,*extra,'test'],logs/'xcodebuild.log')
            summary=json.loads(run(['xcrun','xcresulttool','get','test-results','summary','--path',result,'--format','json'],logs/'summary.json'))
            tree=json.loads(run(['xcrun','xcresulttool','get','test-results','tests','--path',result,'--format','json'],logs/'tests.json'))
            cases=validate_results(summary,tree,expected)
            devices=[{k:v for k,v in d['device'].items() if k in ('platform','architecture','osVersion','modelName')} for d in summary['devicesAndConfigurations']]
            report['platforms'].append({'platform':platform,'cases':cases,'devices':devices,'passed':True})
            run(['xcrun','xcresulttool','export','attachments','--path',result,'--output-path',logs/'attachments'],logs/'attachments.log')
        report['passed']=True
    finally:
        (out/'evidence.json').write_text(json.dumps(report,indent=2)+'\n')
        if owned:
            subprocess.run(['xcrun','simctl','shutdown',owned],capture_output=True)
            subprocess.run(['xcrun','simctl','delete',owned],capture_output=True)
    print(f'Sample app acceptance passed: {out}/evidence.json')

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,subprocess.TimeoutExpired) as error:raise SystemExit(str(error))
