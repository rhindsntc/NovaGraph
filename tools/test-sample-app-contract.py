#!/usr/bin/env python3
import sys
import tempfile
import unittest
from pathlib import Path
from sample_app import run, validate_results

class SampleAcceptanceContracts(unittest.TestCase):
    def test_command_failure_reports_exit_code_and_log_error(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'xcodebuild.log'
            command = [sys.executable, '-c',
                'import sys; print("error: save action did not finish"); '
                '[print(f"build line {i}") for i in range(100)]; sys.exit(7)']
            with self.assertRaises(ValueError) as failure:
                run(command, log)
            message = str(failure.exception)
            self.assertIn('exit code 7', message)
            self.assertIn('error: save action did not finish', message)
            self.assertIn(str(log), message)
            self.assertNotIn('build line 0\n', message)
            self.assertIn('build line 0\n', log.read_text())

    def report(self):
        return {'result':'Passed','totalTestCount':1,'passedTests':1,'failedTests':0,'skippedTests':0,'expectedFailures':0}, {'testNodes':[{'nodeType':'Test Case','nodeIdentifier':'SampleUITests/testReopen()','result':'Passed'}]}
    def test_named_ui_case_must_match_inventory(self):
        summary,tree=self.report()
        with self.assertRaises(ValueError):validate_results(summary,tree,{'SampleUITests/testOther()'})
    def test_skipped_ui_case_cannot_pass(self):
        summary,tree=self.report();summary['skippedTests']=1
        with self.assertRaises(ValueError):validate_results(summary,tree,{'SampleUITests/testReopen()'})
    def test_failed_ui_case_cannot_pass(self):
        summary,tree=self.report();tree['testNodes'][0]['result']='Failed'
        with self.assertRaises(ValueError):validate_results(summary,tree,{'SampleUITests/testReopen()'})
    def test_passed_named_case_is_recorded(self):
        summary,tree=self.report()
        self.assertEqual(validate_results(summary,tree,{'SampleUITests/testReopen()'}),['SampleUITests/testReopen()'])

if __name__=='__main__':unittest.main()
