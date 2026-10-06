#!/usr/bin/env python3
"""Read-only candidate evidence preflight; never publishes or certifies production."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone
import apple_artifacts as apple

ROOT = Path(__file__).resolve().parents[1]

def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''): digest.update(block)
    return digest.hexdigest()

def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise ValueError('duplicate JSON key: '+key)
        result[key] = value
    return result

def read_json(path):
    value = json.loads(Path(path).read_text(), object_pairs_hook=unique_object)
    if not isinstance(value,dict): raise ValueError('expected JSON object: '+str(path))
    return value

def git(root, *args):
    return subprocess.check_output(['git','-C',str(root),*args],text=True).strip()

def identity(root):
    revision = git(root,'rev-parse','HEAD')
    paths = subprocess.check_output(['git','-C',str(root),'ls-files','-z']).decode().split('\0')
    digest = hashlib.sha256()
    for name in sorted(filter(None,paths)):
        path = apple.confined(root,name)
        if path.is_symlink() or not path.is_file():
            content = 'missing-or-symlink'
        else: content = str(path.stat().st_mode & 0o777)+':'+sha256(path)
        digest.update((name+'\0'+content+'\0').encode())
    return dict(revision=revision,tree=git(root,'rev-parse','HEAD^{tree}'),
                dirty=bool(git(root,'status','--porcelain','--untracked-files=all')),
                sourceDigest=digest.hexdigest())

def required_checks(root):
    catalog = read_json(root/'tests/catalog.json')['tests']
    ids = lambda language: {t['id'] for t in catalog if t['language']==language}
    if not ids('cpp') or not ids('swift') or not ids('docs'): raise ValueError('empty required test inventory')
    return {
        'native-debug':ids('cpp'),'native-release':ids('cpp'),'swift':ids('swift'),
        'documentation-execution':ids('docs')|{'web-content','web-ui','web-live','web-build'},
        'recovery':{'process-crash','modeled-power-loss','maintenance-interruption'},
        'model':{t['id'] for t in catalog if t['language']=='cpp' and t['suite']=='model'},
        'sanitizers':{'native-asan-ubsan','native-tsan','swift-asan','swift-tsan','linux-leaks'},
        'source-package':{'debug-consumer','release-consumer','hosted-install'},
        'performance':{'reviewed-budgets','native-candidate','swift-candidate','disk-candidate',
                       'distribution-size','linked-app-size','reference-acceptance'},
        'rollback':{'package-site-rollback','backup-restore-rehearsal','compatibility-review'},
    }

def validate_receipt(entry, directory, candidate, required):
    gate = entry['gate']
    if gate not in required: raise ValueError('unsupported receipt gate: '+gate)
    path = apple.confined(directory,entry['path'])
    if sha256(path)!=entry['sha256']: raise ValueError('receipt hash differs from index')
    record = read_json(path)
    if type(record.get('schemaVersion')) is not int or record['schemaVersion']!=1:
        raise ValueError('unsupported receipt schema')
    if record.get('gate')!=gate or record.get('revision')!=candidate['revision'] or record.get('dirty') is not False:
        raise ValueError('receipt belongs to dirty or foreign candidate')
    if record.get('outcome')!='passed': raise ValueError('receipt outcome is not passed')
    checks = record.get('checks')
    if not isinstance(checks,list) or not checks: raise ValueError('missing named checks')
    names = [c['id'] for c in checks]
    if len(names)!=len(set(names)) or set(names)!=required[gate]:
        raise ValueError('receipt check inventory differs from required checks')
    if any(c.get('outcome')!='passed' for c in checks):
        raise ValueError('required check failed, skipped or has a known failure')
    artifacts = record.get('artifacts')
    if not isinstance(artifacts,list) or not artifacts: raise ValueError('missing raw evidence artifacts')
    names = [a['path'] for a in artifacts]
    if len(names)!=len(set(names)): raise ValueError('duplicate raw evidence artifact')
    for artifact in artifacts:
        if sha256(apple.confined(directory,artifact['path']))!=artifact['sha256']:
            raise ValueError('raw evidence artifact hash differs')
    return dict(receiptSha256=entry['sha256'],checks=len(checks),artifacts=artifacts)

def preflight(root, output, version=None, evidence_index=None, apple_bundle=None):
    root = Path(root).resolve(); output = Path(output).resolve()
    output.mkdir(parents=True,exist_ok=False)
    candidate = identity(root) # Preserve identity BEFORE any validator executes.
    required = required_checks(root)
    manifest = dict(schemaVersion=1,stage='local-preflight',version=version,**candidate,
                    gateToolSha256=sha256(Path(__file__)),documentation=None,artifacts=[],evidence=[])
    manifest['format'] = {}
    for name, filename, pattern in [
        ('rootCatalogRecord','cpp/include/graphdb/BinaryEncoding.hpp',r'kFormatVersion\s*=\s*(\d+)'),
        ('wal','cpp/include/graphdb/Wal.hpp',r'format_version\s*=\s*(\d+)')]:
        match = re.search(pattern,(root/filename).read_text())
        if not match: raise ValueError('unable to read current component version: '+name)
        manifest['format'][name] = int(match[1])
    manifest['cAbi'] = dict(headerSha256=sha256(root/'cpp/include/graphdb/CGraphDB.h'),
                           exportedSymbols=sorted(apple.abi_symbols((root/'cpp/include/graphdb/CGraphDB.h').read_text())))
    site = read_json(root/'docs-web/content/site.json')
    manifest['platforms'] = site['platforms'] # Declared targets; preserve qualification wording.
    gates = []
    def gate(name, status, reason, **details):
        gates.append(dict(id=name,status=status,reason=reason,**details))
    gate('candidate-source','rejected' if candidate['dirty'] else 'passed',
         'uncommitted candidate changes' if candidate['dirty'] else 'clean committed candidate')
    gate('candidate-version','passed' if version else 'missing',
         'requested candidate version; tag not created' if version else 'candidate version not selected')
    catalog = read_json(root/'tests/catalog.json')['tests']
    known = [t['id'] for t in catalog if t['language'] in ('cpp','swift') and t['owner']]
    gate('known-regressions','rejected' if known else 'passed',
         'known regression owners remain' if known else 'no declared native/Swift known regressions',testIds=known)
    command = ['node',str(root/'docs-web/scripts/generate-docs.mjs'),'--check']
    with (output/'documentation-parity.log').open('w') as log:
        try:
            code = subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT,timeout=90).returncode
            reason = 'real documentation generator and recorded-input parity'
        except (OSError,subprocess.TimeoutExpired) as error:
            code = -1;reason = str(error);log.write(reason+'\n')
    try:
        docs = read_json(root/'docs-web/src/data/generated/docs.json')
        manifest['documentation'] = dict(sourceDigest=docs['sourceDigest'],generatedSha256=sha256(root/'docs-web/src/data/generated/docs.json'),
                                        recordedExamplesSha256=sha256(root/'examples/recorded-results.json'))
    except (OSError,ValueError,KeyError,TypeError) as error:
        code = -1;reason = 'invalid generated documentation: '+str(error)
    gate('documentation-parity','passed' if code==0 else 'rejected',reason,
         exitCode=code,logSha256=sha256(output/'documentation-parity.log'))
    entries = {}
    if evidence_index:
        try:
            index = read_json(evidence_index)
            if type(index.get('schemaVersion')) is not int or index['schemaVersion']!=1 or not isinstance(index.get('receipts'),list):
                raise ValueError('unsupported evidence index')
            for entry in index['receipts']:
                name = entry['gate']
                if name not in required or name in entries: raise ValueError('unsupported or duplicate receipt gate: '+name)
                entries[name] = entry
            manifest['evidenceIndexSha256'] = sha256(evidence_index)
        except (OSError,ValueError,KeyError,TypeError) as error:
            gate('evidence-index','rejected',str(error));entries = {}
    for name in required:
        if name not in entries:
            gate(name,'missing','candidate-bound receipt unavailable',requiredChecks=sorted(required[name]));continue
        try:
            result = validate_receipt(entries[name],Path(evidence_index).resolve().parent,candidate,required)
            manifest['evidence'].append(dict(gate=name,**result))
            gate(name,'accepted','receipt identity, exact check inventory and file hashes validated',**result)
        except (OSError,ValueError,KeyError,TypeError) as error:
            gate(name,'rejected',str(error))
    if apple_bundle:
        try:
            artifact = apple.verify_bundle(apple_bundle) # Existing full matrix/header/archive-object checks.
            if artifact.get('revision')!=candidate['revision'] or artifact.get('dirty') is not False or artifact.get('version')!=version:
                raise ValueError('Apple bundle belongs to dirty or foreign candidate/version')
            manifest['artifacts'].append(dict(kind='apple',manifestSha256=sha256(Path(apple_bundle)/'artifact-manifest.json'),
                                             files=artifact['files'],slices=artifact['slices']))
            if artifact['qualification'].get('releaseReady') is not True:
                raise ValueError('Apple minimum-runtime/device qualification incomplete')
            gate('apple-artifacts','accepted','existing complete bundle verifier and runtime qualification passed')
        except (OSError,ValueError,KeyError,TypeError,AttributeError,subprocess.SubprocessError) as error:
            gate('apple-artifacts','rejected',str(error))
    else: gate('apple-artifacts','missing','candidate Apple bundle and complete runtime qualification unavailable')
    # These acceptance protocols are not implemented/qualified by this first slice.
    # No self-declared receipt can waive either gate or turn this preflight into certification.
    gate('quality-qualification','pending','T22 full quality campaigns, sustained soak and frozen-revision nightlies remain incomplete')
    gate('final-release-review','pending','final release/tag, publication and rollback signoff are separate work')
    end = identity(root)
    gate('source-stability','passed' if end==candidate else 'rejected',
         'candidate identity unchanged during validators' if end==candidate else 'candidate changed during validators',observed=end)
    report = dict(schemaVersion=1,stage='local-preflight',createdAt=datetime.now(timezone.utc).isoformat(),
                  revision=candidate['revision'],decision='no-go',productionQualified=False,
                  scope='mechanical evidence preflight; receipts require trusted test producers; not production certification',
                  gates=gates,blockers=[g['id'] for g in gates if g['status'] not in ('passed','accepted')])
    (output/'candidate-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    report['manifestSha256'] = sha256(output/'candidate-manifest.json')
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

def candidate_version(value):
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?',value):
        raise argparse.ArgumentTypeError('version must be a numeric semantic version with optional prerelease')
    return value

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--version',type=candidate_version)
    parser.add_argument('--evidence-index',type=Path)
    parser.add_argument('--apple-bundle',type=Path)
    args = parser.parse_args()
    try:
        report = preflight(args.root,args.output,args.version,args.evidence_index,args.apple_bundle)
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError) as error:
        print('Preflight input/setup error: '+str(error),file=sys.stderr);return 2
    print(json.dumps(dict(decision=report['decision'],blockers=report['blockers'],output=str(args.output))))
    return 1 # This first slice deliberately cannot certify a production go.
if __name__=='__main__':sys.exit(main())
