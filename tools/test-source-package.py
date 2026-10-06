#!/usr/bin/env python3
"""Resolve a tracked-source candidate via Git URL with isolated SwiftPM state."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TAG = '0.0.0-t18-candidate'
OPTIONS = argparse.Namespace(revision=None, output=None)
EVIDENCE = []

def run(args, cwd, env=None, timeout=600):
    result = subprocess.run(args, cwd=cwd, env=env, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(f'{args[0]} failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
    return result.stdout.strip()

class SourcePackageContracts(unittest.TestCase):
    def check_consumer(self, configuration):
        with tempfile.TemporaryDirectory(prefix='nova-source-package-') as directory:
            temp = Path(directory).resolve(); repo = temp/'NovaCandidate'; repo.mkdir()
            if OPTIONS.revision:
                # Only committed files enter the candidate; build products/caches cannot.
                archive = subprocess.Popen(['git','archive',OPTIONS.revision], cwd=ROOT, stdout=subprocess.PIPE)
                subprocess.run(['tar','-x','-C',str(repo)],stdin=archive.stdout,check=True)
                archive.stdout.close()
                self.assertEqual(archive.wait(),0)
            else:
                names = run(['git','ls-files','-z','--cached','--others','--exclude-standard'],ROOT).split('\0')
                for name in names:
                    source=ROOT/name
                    if name and source.is_file():
                        target=repo/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
            self.assertFalse((repo/'build').exists());self.assertFalse((repo/'.build').exists())
            run(['git','init','-q'],repo)
            # Snapshot/archive selection already excludes build products. Preserve
            # tracked inputs even when their paths match the repository ignore rules.
            run(['git','add','--force','.'],repo)
            run(['git','-c','user.name=Nova acceptance','-c','user.email=acceptance@example.invalid','commit','-qm','Source candidate'],repo)
            run(['git','tag',TAG],repo)
            candidate=run(['git','rev-parse','HEAD'],repo)
            if OPTIONS.revision:
                # Includes tracked-but-ignored inputs such as devtools/Package.resolved.
                self.assertEqual(run(['git','rev-parse','HEAD^{tree}'],repo),
                                 run(['git','rev-parse',OPTIONS.revision+'^{tree}'],ROOT),
                                 'Candidate must preserve every committed source file and mode')
            consumer=temp/'Consumer';(consumer/'Sources/IntegrationSmoke').mkdir(parents=True)
            shutil.copy2(repo/'examples/IntegrationSmoke/Sources/IntegrationSmoke/main.swift',consumer/'Sources/IntegrationSmoke/main.swift')
            (consumer/'Package.swift').write_text('// swift-tools-version: 6.0\nimport PackageDescription\nlet package = Package(name: "Consumer", platforms: [.macOS(.v13)], dependencies: [.package(url: '+json.dumps(repo.as_uri())+', exact: "'+TAG+'")], targets: [.executableTarget(name: "IntegrationSmoke", dependencies: [.product(name: "GraphDBKit", package: "NovaCandidate")])])\n')
            env={key:value for key,value in os.environ.items() if key not in {'CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','DYLD_LIBRARY_PATH','LD_LIBRARY_PATH','SDKROOT','SWIFT_EXEC','SWIFT_DRIVER_SWIFT_FRONTEND_EXEC','MACOSX_DEPLOYMENT_TARGET'}}
            command=['swift','run','--cache-path',str(temp/'cache'),'--config-path',str(temp/'config'),'--security-path',str(temp/'security'),'--scratch-path',str(temp/'scratch'),'--disable-dependency-cache','--manifest-cache','none','-c',configuration]
            created=run(command+['IntegrationSmoke','create',str(temp/'database')],consumer,env)
            reopened=run(command+['--skip-build','IntegrationSmoke','reopen',str(temp/'database')],consumer,env)
            self.assertEqual(created,'IntegrationSmoke create: values, query and path passed')
            self.assertEqual(reopened,'IntegrationSmoke reopen: values, query and path passed')
            resolved=json.loads((consumer/'Package.resolved').read_text())
            self.assertEqual(len(resolved['pins']),1, 'Core package must not fetch extra source/binary dependencies')
            self.assertEqual(resolved['pins'][0]['state']['revision'],candidate)
            self.assertEqual(resolved['pins'][0]['state']['version'],TAG)
            minimumOS = None
            if sys.platform == 'darwin':
                binaryDir = run(['swift','build',*command[2:],'--show-bin-path'],consumer,env)
                buildInfo = run(['xcrun','vtool','-show-build',str(Path(binaryDir)/'IntegrationSmoke')],consumer,env)
                minimumOS = re.search(r'minos\s+([0-9.]+)',buildInfo).group(1)
                self.assertEqual(minimumOS,'13.0', 'Consumer must retain the declared minimum deployment target')
            EVIDENCE.append(dict(configuration=configuration,candidateRevision=candidate,candidateTag=TAG,sourceTree=run(['git','rev-parse','HEAD^{tree}'],repo),transport='isolated file:// Git URL',created=created,reopened=reopened,isolatedCache=True,manualLinkerFlags=False,minimumOS=minimumOS))

    def test_debug_url_consumer_creates_and_reopens(self): self.check_consumer('debug')
    def test_release_url_consumer_creates_and_reopens(self): self.check_consumer('release')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision',help='Committed revision to archive; default snapshots tracked and unignored source for development')
    parser.add_argument('--output',type=Path)
    OPTIONS,rest=parser.parse_known_args()
    program=unittest.main(argv=[__file__,*rest],exit=False)
    if OPTIONS.output:
        OPTIONS.output.parent.mkdir(parents=True,exist_ok=True)
        OPTIONS.output.write_text(json.dumps(dict(schemaVersion=1,revision=run(['git','rev-parse',OPTIONS.revision or 'HEAD'],ROOT),dirty=bool(run(['git','status','--porcelain'],ROOT)),swift=run(['swift','--version'],ROOT),checks=EVIDENCE,passed=program.result.wasSuccessful()),indent=2)+'\n')
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
