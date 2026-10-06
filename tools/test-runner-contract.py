#!/usr/bin/env python3
"""Exercise the runner's exit status in Debug and Release, including XFAIL isolation."""
import pathlib
import subprocess
import tempfile
import sys
root = pathlib.Path(__file__).resolve().parents[1]
source = r'''
#include "TestSupport.hpp"
NOVA_TEST(always_active, "failure", "") { int count=0; CHECK(++count==2); }
NOVA_TEST(expected, "expected", "T07") { CONTRACT("expected",false); }
NOVA_TEST(unrelated, "unrelated", "T07") { CHECK(false); }
NOVA_TEST(wrong_contract, "wrong", "T07") { CONTRACT("another_case",false); }
NOVA_TEST(unexpected_pass, "xpass", "T07") { CONTRACT("unexpected_pass",true); }
NOVA_TEST(ordinary_pass, "pass", "") { CHECK(true); }
'''
for mode, flags in [('Debug', ['-O0']), ('Release', ['-O3', '-DNDEBUG'])]:
    with tempfile.TemporaryDirectory(prefix='nova-runner-') as temp:
        directory=pathlib.Path(temp); fixture=directory/'fixture.cpp'; fixture.write_text(source)
        binary=directory/'runner'
        subprocess.run(['c++','-std=c++20',*flags,'-I'+str(root/'cpp/include'),'-I'+str(root/'cpp/tests'),str(root/'cpp/tests/test_engine.cpp'),str(fixture),'-o',str(binary)],check=True)
        for suite, strict, code, marker in [('failure',False,1,'FAIL'),('expected',False,0,'XFAIL'),('expected',True,1,'FAIL'),('unrelated',False,1,'unexpected/setup'),('wrong',False,1,'FAIL'),('xpass',False,1,'XPASS'),('pass',False,0,'PASS'),('absent',False,1,'0 passed')]:
            command=[str(binary),'--suite',suite]+(['--strict'] if strict else [])
            result=subprocess.run(command,text=True,capture_output=True)
            if result.returncode!=code or marker not in result.stdout:
                sys.exit(f'{mode} {suite}: exit={result.returncode}, output={result.stdout} {result.stderr}')
        print(f'{mode}: 8 runner contracts passed (intentional failures returned nonzero)')
