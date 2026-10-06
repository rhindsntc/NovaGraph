#!/usr/bin/env python3
"""Exercise public export against real disposable Git repositories."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).with_name('export-public.py')

class PublicExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root/'repo'; self.repo.mkdir()
        self.output = self.root/'public.zip'
        self.git('init', '-q')
        (self.repo/'.gitignore').write_text('/docs/\n/build/\n.env\n')
        (self.repo/'README.md').write_text('Public source\n')
        self.git('add', '.')
        self.commit()

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.repo, stderr=subprocess.STDOUT)

    def commit(self):
        self.git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'Snapshot')

    def export(self):
        return subprocess.run([sys.executable, str(SCRIPT), '--output', str(self.output)], cwd=self.repo, capture_output=True, text=True)

    def test_ignored_private_files_and_history_never_enter_archive(self):
        for name in ['docs/private.md', 'build/evidence.json', '.env']:
            p=self.repo/name; p.parent.mkdir(exist_ok=True); p.write_text('PRIVATE_SENTINEL')
        result=self.export()
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {'Nova/', 'Nova/.gitignore', 'Nova/README.md'})
            self.assertEqual(archive.read('Nova/README.md'), b'Public source\n')

    def test_tracked_private_document_is_rejected(self):
        p=self.repo/'docs/private.md';p.parent.mkdir();p.write_text('private')
        self.git('add','--force','docs/private.md');self.commit()
        result=self.export()
        self.assertNotEqual(result.returncode,0)
        self.assertIn('private',result.stderr.lower());self.assertFalse(self.output.exists())

    def test_uncommitted_change_cannot_export_stale_source(self):
        (self.repo/'README.md').write_text('new version')
        result=self.export()
        self.assertNotEqual(result.returncode,0)
        self.assertIn('clean',result.stderr.lower());self.assertFalse(self.output.exists())

    def test_untracked_public_file_cannot_be_silently_omitted(self):
        (self.repo/'LICENSE').write_text('public license')
        result=self.export()
        self.assertNotEqual(result.returncode,0)
        self.assertIn('clean',result.stderr.lower())

    def test_public_symlink_to_private_file_is_rejected(self):
        (self.repo/'docs').mkdir();(self.repo/'docs/private.md').write_text('private')
        (self.repo/'public.md').symlink_to('docs/private.md')
        self.git('add','public.md');self.commit()
        result=self.export()
        self.assertNotEqual(result.returncode,0)
        self.assertIn('symlink',result.stderr.lower());self.assertFalse(self.output.exists())

    def test_existing_archive_is_not_overwritten(self):
        self.output.write_bytes(b'preserve')
        result=self.export()
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(self.output.read_bytes(),b'preserve')

if __name__=='__main__':unittest.main()
