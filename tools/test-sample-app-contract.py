#!/usr/bin/env python3
import unittest
from sample_app import validate_results

class SampleAcceptanceContracts(unittest.TestCase):
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
