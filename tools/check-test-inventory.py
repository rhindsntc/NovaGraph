#!/usr/bin/env python3
"""Compare checked-in IDs with the executable runner or Swift's discovery output."""
import json
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
catalog = json.loads((root / 'tests/catalog.json').read_text())['tests']
if len(sys.argv) == 2 and sys.argv[1] == '--release':
    import importlib.util, unittest
    spec=importlib.util.spec_from_file_location('release_contracts',root/'tools/test-release-preflight.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(module.ReleasePreflightContracts)
    actual=sorted('devtools.'+'.'.join(test.id().split('.')[-2:]) for test in suite)
    expected=sorted(item['id'] for item in catalog if item['language']=='devtools' and item['suite']=='ReleasePreflightContracts')
    if not actual or actual!=expected:sys.exit('Release preflight inventory differs from unittest discovery')
    print(f'Test inventory matches {len(actual)} release preflight contracts.')
    sys.exit(0)
elif len(sys.argv) == 2 and sys.argv[1] == '--performance':
    import importlib.util, unittest
    actual=[]
    for filename,classname in [('test-performance.py','PerformanceContracts'),('test-swift-measurements.py','SwiftMeasurementContracts'),('test-resource-measurements.py','ResourceMeasurementContracts'),('test-performance-gate.py','GateContracts'),('test-performance-diagnostic.py','DiagnosticContracts'),('test-resource-budgets.py','ResourceBudgetContracts')]:
        spec=importlib.util.spec_from_file_location('performance_contracts',root/'tools'/filename)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        suite=unittest.defaultTestLoader.loadTestsFromTestCase(getattr(module,classname))
        actual.extend('performance.'+'.'.join(t.id().split('.')[-2:]) for t in suite)
    actual=sorted(actual)
    expected=sorted(t['id'] for t in catalog if t['language']=='performance')
    if not actual or actual!=expected:sys.exit('Performance inventory differs from unittest discovery')
    print(f'Test inventory matches {len(actual)} performance contracts.')
    sys.exit(0)
elif len(sys.argv) == 2 and sys.argv[1] == '--devtools':
    import importlib.util, unittest
    spec = importlib.util.spec_from_file_location('nova_runner_tests',root/'tools/test-dev-query.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(module.HTTPContracts)
    actual = sorted('devtools.'+'.'.join(test.id().split('.')[-2:]) for test in suite)
    expected = sorted(item['id'] for item in catalog if item['language']=='devtools' and item['suite']=='local_runner')
    if not actual or actual != expected: sys.exit('Developer runner inventory differs from unittest discovery')
    print(f'Test inventory matches {len(actual)} executable HTTP cases.')
    sys.exit(0)
elif len(sys.argv) == 2 and sys.argv[1] == '--package':
    import importlib.util, unittest
    spec=importlib.util.spec_from_file_location('source_package_tests',root/'tools/test-source-package.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(module.SourcePackageContracts)
    actual=sorted('package.'+'.'.join(test.id().split('.')[-2:]) for test in suite)
    expected=sorted(item['id'] for item in catalog if item['language']=='package')
    if not actual or actual!=expected: sys.exit('Source-package inventory differs from unittest discovery')
    print(f'Test inventory matches {len(actual)} executable source-package cases.')
    sys.exit(0)
elif len(sys.argv) == 2 and sys.argv[1] == '--artifacts':
    import importlib.util, unittest
    spec=importlib.util.spec_from_file_location('apple_artifact_tests',root/'tools/test-apple-artifacts.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(module.AppleArtifactContracts)
    actual=sorted('artifact.'+'.'.join(test.id().split('.')[-2:]) for test in suite)
    expected=sorted(item['id'] for item in catalog if item['language']=='artifact')
    if not actual or actual!=expected: sys.exit('Apple artifact inventory differs from unittest discovery')
    print(f'Test inventory matches {len(actual)} Apple artifact contracts.')
    sys.exit(0)
elif len(sys.argv) == 3 and sys.argv[1] == '--sample':
    listing=pathlib.Path(sys.argv[2]).read_text()
    actual=sorted('sample.'+a+'.'+b for a,b in re.findall(r'^NovaSampleTests\.(\w+)/(test\w+)\s*$',listing,re.MULTILINE))
    expected=sorted(t['id'] for t in catalog if t['language']=='sample' and t['suite']=='sample-model')
    if not actual or actual!=expected:sys.exit('Sample model inventory differs from Swift test discovery')
    import importlib.util, unittest
    spec=importlib.util.spec_from_file_location('sample_contracts',root/'tools/test-sample-app-contract.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    tests=unittest.defaultTestLoader.loadTestsFromTestCase(module.SampleAcceptanceContracts)
    harness=sorted('sample.'+'.'.join(t.id().split('.')[-2:]) for t in tests)
    if harness!=sorted(t['id'] for t in catalog if t['language']=='sample' and t['suite']=='sample-harness'):sys.exit('Sample harness inventory differs from unittest discovery')
    print(f'Test inventory matches {len(actual)} sample model and {len(harness)} acceptance contracts.')
    sys.exit(0)
elif len(sys.argv) == 3 and sys.argv[1] == '--swift':
    listing = pathlib.Path(sys.argv[2]).read_text()
    actual = ['swift.' + match[0] + '.' + match[1] for match in
              re.findall(r'^GraphDBKitTests\.(\w+)/(test\w+)\s*$', listing, re.MULTILINE)]
    expected = [item['id'] for item in catalog if item['language'] == 'swift']
    language = 'Swift'
else:
    actual = json.loads(subprocess.check_output([sys.argv[1], '--list-json'], text=True))
    actual = [dict(id='cpp.' + item['id'], suite=item['suite'], owner=item['owner'], language='cpp')
              for item in actual]
    expected = [item for item in catalog if item['language'] == 'cpp']
    actual.sort(key=lambda item: item['id'])
    expected.sort(key=lambda item: item['id'])
    language = 'C++'
matches = sorted(actual) == sorted(expected) if language == 'Swift' else actual == expected
if not matches:
    sys.exit(f'tests/catalog.json does not match the executable {language} test inventory')
if not actual:
    sys.exit(f'No {language} tests discovered')
print(f'Test inventory matches {len(actual)} executable {language} cases.')
