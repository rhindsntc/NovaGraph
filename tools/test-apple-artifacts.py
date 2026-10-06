#!/usr/bin/env python3
"""Contract regressions for complete, truthful Apple artifact evidence."""
import copy
import argparse
import plistlib
import importlib.util
import json
import io
import zipfile
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]

class AppleArtifactContracts(unittest.TestCase):
    def setUp(self):
        path = ROOT/'tools/apple_artifacts.py'
        self.assertTrue(path.exists(), 'Apple artifact validator is not implemented')
        spec = importlib.util.spec_from_file_location('apple_artifacts', path)
        self.api = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.api)

    def rows(self):
        return [dict(id=i, platform=p, variant=v, architectures=a, minimumOS=m, minimumOSByArchitecture=({'arm64':'26.0','arm64_32':'9.0'} if i=='watchos' else {})) for i,p,v,a,m in [
            ('macos','macos','',['arm64','x86_64'],'13.0'),
            ('ios','ios','',['arm64'],'16.0'),
            ('ios-simulator','ios','simulator',['arm64','x86_64'],'16.0'),
            ('catalyst','ios','maccatalyst',['arm64','x86_64'],'16.0'),
            ('tvos','tvos','',['arm64'],'16.0'),
            ('tvos-simulator','tvos','simulator',['arm64','x86_64'],'16.0'),
            ('watchos','watchos','',['arm64','arm64_32'],'9.0'),
            ('watchos-simulator','watchos','simulator',['arm64','x86_64'],'9.0')]]

    def test_missing_watch_slice_cannot_pass(self):
        rows = self.rows(); self.api.validate_matrix(rows)
        with self.assertRaisesRegex(ValueError, 'missing|matrix'):
            self.api.validate_matrix([r for r in rows if r['id'] != 'watchos'])

    def test_wrong_architecture_platform_or_floor_cannot_pass(self):
        for field, value in [('architectures',['arm64']),('platform','ios'),('minimumOS','14.0')]:
            rows=self.rows(); rows[0][field]=value
            with self.subTest(field=field), self.assertRaises(ValueError): self.api.validate_matrix(rows)
        rows=self.rows(); rows[2]['variant']=''
        with self.assertRaises(ValueError): self.api.validate_matrix(rows)

    def test_every_object_load_command_must_match(self):
        good='archive(one.o):\nLoad command 0\n cmd LC_BUILD_VERSION\n cmdsize 24\n platform 7\n minos 16.0\n sdk 27.0\n'
        self.api.validate_load_commands(good+good, 7, '16.0', 2)
        for bad in [good,good+good.replace('minos 16.0','minos 17.0'),good+good.replace('platform 7','platform 2')]:
            with self.subTest(bad=bad), self.assertRaises(ValueError): self.api.validate_load_commands(bad,7,'16.0',2)

    def test_artifact_paths_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve(); (root/'ok').write_text('ok')
            self.assertEqual(self.api.confined(root,'ok'),root/'ok')
            for path in ['../outside','/tmp/outside']:
                with self.assertRaises(ValueError): self.api.confined(root,path)
            (root/'alias').symlink_to('/tmp')
            with self.assertRaises(ValueError): self.api.confined(root,'alias/file')

    def test_missing_or_changed_file_fails_checksum_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve(); (root/'library').write_bytes(b'valid archive')
            records={'library':self.api.sha256(root/'library')}
            self.api.validate_files(root,records)
            (root/'library').write_bytes(b'changed archive')
            with self.assertRaises(ValueError): self.api.validate_files(root,records)
            (root/'library').unlink()
            with self.assertRaises(ValueError): self.api.validate_files(root,records)

    def test_unavailable_runtime_is_a_release_blocker(self):
        rows=self.rows()
        results=[dict(id=r['id'], status='unavailable', reason='runtime missing') for r in rows]
        gate=self.api.qualification(rows,results)
        self.assertFalse(gate['releaseReady']); self.assertEqual(len(gate['blockers']),8)
        with self.assertRaises(ValueError): self.api.require_qualified(gate)

    def test_qualified_label_without_actual_distribution_runs_is_rejected(self):
        rows=self.rows()
        claims=[dict(id=r['id'],status='qualified') for r in rows]
        self.assertFalse(self.api.qualification(rows,claims)['releaseReady'])

    def test_extra_file_cannot_escape_checksum_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'library').write_bytes(b'core')
            records={'library':self.api.sha256(root/'library')}
            (root/'untracked-header.h').write_text('private')
            with self.assertRaises(ValueError): self.api.validate_files(root,records)

    def test_foreign_or_dirty_consumer_evidence_is_rejected(self):
        manifest={'revision':'a'*40,'dirty':False,'files':{'library':'b'*64},'slices':self.rows()}
        report={'revision':'a'*40,'dirty':False,'artifactContentDigest':'wrong','runtimeEvidence':[],'builds':[]}
        with self.assertRaises(ValueError):self.api.validate_evidence(manifest,report)
        report['artifactContentDigest']=self.api.content_digest(manifest)
        report['dirty']=True
        with self.assertRaises(ValueError):self.api.validate_evidence(manifest,report)

    def test_newer_runtime_does_not_prove_minimum_runtime(self):
        rows=self.rows();records=[]
        for row in rows:
            for arch in row['architectures']:
                for distribution in ('source','binary'):
                    records.append(dict(id=row['id'],architecture=arch,distribution=distribution,status='passed',kind='simulator' if row['variant']=='simulator' else ('native' if row['id'] in ('macos','catalyst') else 'device'),osVersion=('13.0' if row['id']=='catalyst' else row['minimumOSByArchitecture'].get(arch,row['minimumOS'])),check='create-query-close-reopen'))
        self.assertTrue(self.api.qualification(rows,records)['releaseReady'])
        records[0]['osVersion']='27.0'
        self.assertFalse(self.api.qualification(rows,records)['releaseReady'])

    def test_catalyst_16_requires_macos_13_runtime(self):
        row=next(r for r in self.rows() if r['id']=='catalyst')
        runs=[dict(id='catalyst',architecture=a,distribution=d,kind='native',osVersion='13.0',status='passed',check='create-query-close-reopen') for a in ('arm64','x86_64') for d in ('source','binary')]
        self.assertTrue(self.api.qualification([row],runs)['releaseReady'])

    def test_zip_must_contain_the_verified_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);src=root/'NovaGraph';src.mkdir();(src/'library').write_bytes(b'verified')
            self.api.archive_tree(src,root/'candidate.zip')
            self.api.validate_archive(src,root/'candidate.zip')
            (src/'library').write_bytes(b'new contents')
            with self.assertRaises(ValueError):self.api.validate_archive(src,root/'candidate.zip')

    def test_same_candidate_device_evidence_merges_with_complete_builds(self):
        manifest={'revision':'a'*40,'dirty':False,'files':{'library':'b'*64},'slices':self.rows()}
        common=dict(revision=manifest['revision'],sourceRevision=manifest['revision'],dirty=False,artifactContentDigest=self.api.content_digest(manifest),runtimeEvidence=[])
        builds=[dict(id=r['id'],distribution=d,architectures=r['architectures'],status='passed') for r in self.rows() for d in ('source','binary')]
        full=common|dict(builds=builds);partial=common|dict(builds=builds[2:4])
        merged=self.api.merge_evidence(manifest,[full,partial])
        self.assertEqual(len(merged['builds']),16)
        partial['revision']='c'*40
        with self.assertRaises(ValueError):self.api.merge_evidence(manifest,[full,partial])

    def consumer_api(self):
        spec=importlib.util.spec_from_file_location('apple_consumers',ROOT/'tools/test-apple-consumers.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        return module

    def test_clean_foreign_source_in_development_mode_cannot_qualify(self):
        api=self.consumer_api();manifest=dict(revision='a'*40,dirty=False)
        state=api.source_state(manifest,'b'*40,False,True)
        self.assertEqual(state['sourceRevision'],'b'*40)
        self.assertTrue(state['dirty'])
        self.assertTrue(api.source_state(manifest,'a'*40,False,True)['dirty'])
        self.assertFalse(api.source_state(manifest,'a'*40,False,False)['dirty'])

    def test_minimum_simulator_is_selected_alongside_newer_runtime(self):
        api=self.consumer_api()
        runtimes=[dict(name='iOS 27.0',version='27.0',identifier='ios27',isAvailable=True),dict(name='iOS 16.0',version='16.0',identifier='ios16',isAvailable=True)]
        self.assertEqual(api.select_runtime(runtimes,'iOS','16.0')['identifier'],'ios16')
        self.assertEqual(api.select_runtime(runtimes,'iOS','16.0','ios27')['identifier'],'ios27')
        with self.assertRaises(ValueError):api.select_runtime(runtimes,'iOS','16.0','absent')
        runtimes[1]['isAvailable']=False
        self.assertEqual(api.select_runtime(runtimes,'iOS','16.0')['identifier'],'ios27')

    def test_xcode_slice_order_does_not_change_artifact_metadata(self):
        import plistlib
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'Info.plist'; rows=[dict(LibraryIdentifier='ios-arm64'),dict(LibraryIdentifier='macos-arm64_x86_64')]
            p.write_bytes(plistlib.dumps(dict(AvailableLibraries=rows)))
            self.api.normalize_framework_info(p);first=p.read_bytes()
            p.write_bytes(plistlib.dumps(dict(AvailableLibraries=list(reversed(rows)))))
            self.api.normalize_framework_info(p)
            self.assertEqual(first,p.read_bytes())

    def test_zip_is_stable_and_preserves_license(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve(); src=root/'source';src.mkdir();(src/'LICENSE').write_text('license bytes')
            self.api.archive_tree(src,root/'one.zip'); self.api.archive_tree(src,root/'two.zip')
            self.assertEqual((root/'one.zip').read_bytes(),(root/'two.zip').read_bytes())
            import zipfile
            with zipfile.ZipFile(root/'one.zip') as archive:
                self.assertEqual(archive.read('source/LICENSE'),b'license bytes')

    def size_fixture(self, directory):
        root=Path(directory);bundle=root/'NovaGraph';bundle.mkdir()
        (bundle/'library.a').write_bytes(b'core archive')
        (bundle/'LICENSE').write_bytes(b'license')
        compressed=io.BytesIO()
        with zipfile.ZipFile(compressed,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
            z.write(bundle/'library.a','libnovagraphdb.a')
        slice_row=dict(id='ios',installedBytes=12,compressedBytes=len(compressed.getvalue()),sha256=self.api.sha256(bundle/'library.a'))
        manifest=dict(schemaVersion=1,revision='a'*40,dirty=False,
                      files={name:self.api.sha256(bundle/name) for name in ('library.a','LICENSE')},
                      slices=[slice_row],qualification={'releaseReady':False,'blockers':['runtime unavailable']})
        (bundle/'artifact-manifest.json').write_text(json.dumps(manifest))
        archive=root/'NovaGraph.zip';self.api.archive_tree(bundle,archive)
        (root/'SHA256SUMS').write_text(self.api.sha256(archive)+'  '+archive.name+'\n')
        return bundle,manifest,archive

    def test_resource_sizes_measure_verified_files_without_self_manifest(self):
        self.assertTrue(callable(getattr(self.api,'resource_size_report',None)), 'resource size reporting is missing')
        with tempfile.TemporaryDirectory() as directory:
            bundle,manifest,archive=self.size_fixture(directory)
            before=copy.deepcopy(manifest)
            sizes=self.api.resource_size_report(bundle,manifest,archive)
            self.assertEqual(sizes['schemaVersion'],1)
            self.assertEqual(sizes['bundleLogicalBytes'],19)
            self.assertEqual(sizes['fileCount'],2)
            self.assertEqual(sizes['archiveBytes'],archive.stat().st_size)
            self.assertEqual(sizes['files']['library.a']['bytes'],12)
            self.assertEqual(sizes['files']['LICENSE']['bytes'],7)
            self.assertEqual(sizes['slices'][0]['id'],'ios')
            self.assertEqual(sizes['slices'][0]['installedBytes'],12)
            self.assertNotIn('artifact-manifest.json',sizes['files'])
            self.assertIn('artifact-manifest.json',sizes['scope'])
            self.assertEqual(sizes['revision'],'a'*40)
            self.assertEqual(sizes['artifactContentDigest'],self.api.content_digest(manifest))
            self.assertEqual(manifest,before)
            self.api.validate_resource_sizes(manifest,sizes)

    def test_resource_sizes_reject_changed_missing_or_untracked_bundle_files(self):
        self.assertTrue(callable(getattr(self.api,'resource_size_report',None)), 'resource size reporting is missing')
        for change in ('changed','missing','extra'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as directory:
                bundle,manifest,archive=self.size_fixture(directory)
                if change=='changed':(bundle/'library.a').write_bytes(b'wrongarchive')
                elif change=='missing':(bundle/'library.a').unlink()
                else:(bundle/'extra').write_bytes(b'x')
                with self.assertRaises(ValueError):self.api.resource_size_report(bundle,manifest,archive)

    def test_resource_sizes_reject_dirty_foreign_or_invalid_revision(self):
        self.assertTrue(callable(getattr(self.api,'resource_size_report',None)), 'resource size reporting is missing')
        for field,value in [('dirty',True),('revision','b'*40),('revision','unknown')]:
            with self.subTest(field=field,value=value),tempfile.TemporaryDirectory() as directory:
                bundle,manifest,archive=self.size_fixture(directory)
                manifest[field]=value
                with self.assertRaises(ValueError):self.api.resource_size_report(bundle,manifest,archive)

    def test_resource_sizes_reject_unsafe_or_missing_hash_inventory(self):
        self.assertTrue(callable(getattr(self.api,'resource_size_report',None)), 'resource size reporting is missing')
        for inventory in ({},{'../outside':'a'*64},{'/tmp/outside':'a'*64},{'library.a':'bad'}):
            with self.subTest(inventory=inventory),tempfile.TemporaryDirectory() as directory:
                bundle,manifest,archive=self.size_fixture(directory);manifest['files']=inventory
                with self.assertRaises(ValueError):self.api.resource_size_report(bundle,manifest,archive)
        with tempfile.TemporaryDirectory() as directory:
            bundle,manifest,archive=self.size_fixture(directory)
            (bundle/'library.a').unlink();(bundle/'library.a').symlink_to(bundle/'LICENSE')
            manifest['files']['library.a']=self.api.sha256(bundle/'library.a')
            with self.assertRaises(ValueError):self.api.resource_size_report(bundle,manifest,archive)

    def test_resource_sizes_reject_stale_zip_and_checksum(self):
        self.assertTrue(callable(getattr(self.api,'resource_size_report',None)), 'resource size reporting is missing')
        for change in ('zip','checksum','missing'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as directory:
                bundle,manifest,archive=self.size_fixture(directory)
                if change=='zip':
                    with zipfile.ZipFile(archive,'a') as z:z.writestr('foreign',b'bad')
                    (archive.parent/'SHA256SUMS').write_text(self.api.sha256(archive)+'  '+archive.name+'\n')
                elif change=='checksum':(archive.parent/'SHA256SUMS').write_text('wrong')
                else:archive.unlink()
                with self.assertRaises((ValueError,OSError)):self.api.resource_size_report(bundle,manifest,archive)

    def test_resource_size_metadata_rejects_missing_foreign_and_invalid_values(self):
        self.assertTrue(callable(getattr(self.api,'resource_size_report',None)), 'resource size reporting is missing')
        with tempfile.TemporaryDirectory() as directory:
            bundle,manifest,archive=self.size_fixture(directory)
            valid=self.api.resource_size_report(bundle,manifest,archive)
            for field,value in [('schemaVersion',2),('bundleLogicalBytes',20),('bundleLogicalBytes',True),
                                ('archiveBytes',0),('archiveBytes',1.5),('fileCount',3),('revision','b'*40),
                                ('dirty',True),('artifactContentDigest','c'*64),('archiveSha256','bad'),
                                ('scope','installed app bytes'),('files',{})]:
                with self.subTest(field=field,value=value):
                    invalid=copy.deepcopy(valid);invalid[field]=value
                    with self.assertRaises(ValueError):self.api.validate_resource_sizes(manifest,invalid)
            inconsistent=copy.deepcopy(manifest);inconsistent['slices'][0]['installedBytes']=13
            invalid=copy.deepcopy(valid);invalid['slices'][0]['installedBytes']=13
            with self.assertRaises(ValueError):self.api.validate_resource_sizes(inconsistent,invalid)
            for field in valid:
                with self.subTest(missing=field):
                    invalid=copy.deepcopy(valid);del invalid[field]
                    with self.assertRaises(ValueError):self.api.validate_resource_sizes(manifest,invalid)
            for field,value in [('bytes',-1),('bytes',True),('sha256','d'*64)]:
                with self.subTest(file_field=field):
                    invalid=copy.deepcopy(valid);invalid['files']['library.a'][field]=value
                    with self.assertRaises(ValueError):self.api.validate_resource_sizes(manifest,invalid)

    def test_slice_sizes_reject_missing_or_stale_compressed_bytes(self):
        self.assertTrue(callable(getattr(self.api,'validate_slice_sizes',None)), 'slice size verification is missing')
        with tempfile.TemporaryDirectory() as directory:
            library=Path(directory)/'library.a';library.write_bytes(b'core archive')
            archive=Path(directory)/'slice.zip'
            with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
                z.write(library,'libnovagraphdb.a')
            row=dict(installedBytes=12,compressedBytes=archive.stat().st_size,sha256=self.api.sha256(library))
            self.api.validate_slice_sizes(library,row)
            for field in ('installedBytes','compressedBytes','sha256'):
                with self.subTest(field=field):
                    invalid=copy.deepcopy(row);del invalid[field]
                    with self.assertRaises(ValueError):self.api.validate_slice_sizes(library,invalid)
                    invalid=copy.deepcopy(row);invalid[field]=1
                    with self.assertRaises(ValueError):self.api.validate_slice_sizes(library,invalid)

    def test_verify_report_emits_sizes_without_runtime_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,manifest,archive=self.size_fixture(directory);report=Path(directory)/'report.json'
            # Only substitute the platform/ABI inspection, which requires real
            # Apple object archives; report file/hash/ZIP verification stays real.
            with patch.object(self.api,'verify_bundle',return_value=copy.deepcopy(manifest)), \
                 patch('sys.argv',['apple_artifacts.py','verify',str(bundle),'--report',str(report)]), \
                 patch('sys.stdout',new_callable=io.StringIO),patch('sys.stderr',new_callable=io.StringIO):
                self.api.main()
            recorded=json.loads(report.read_text())
            self.assertIn('resourceSizes',recorded)
            self.assertEqual(recorded['resourceSizes']['bundleLogicalBytes'],19)
            self.assertEqual(recorded['archive']['bytes'],archive.stat().st_size)
            self.assertEqual(recorded['qualification'],manifest['qualification'])
            self.assertNotIn('consumerEvidence',recorded)
            self.api.validate_resource_sizes(recorded,recorded['resourceSizes'])

    def test_verify_report_cannot_modify_verified_bundle(self):
        for destination in ('bundle','archive','checksum'):
            with self.subTest(destination=destination),tempfile.TemporaryDirectory() as directory:
                bundle,manifest,archive=self.size_fixture(directory)
                report={'bundle':bundle/'report.json','archive':archive,'checksum':archive.parent/'SHA256SUMS'}[destination]
                before=report.read_bytes() if report.exists() else None
                with patch.object(self.api,'verify_bundle',return_value=copy.deepcopy(manifest)), \
                     patch('sys.argv',['apple_artifacts.py','verify',str(bundle),'--report',str(report)]), \
                 patch('sys.stdout',new_callable=io.StringIO),patch('sys.stderr',new_callable=io.StringIO):
                    with self.assertRaises(SystemExit) as error:self.api.main()
                self.assertEqual(error.exception.code,1)
                self.assertEqual(report.read_bytes() if report.exists() else None,before)
                self.api.validate_files(bundle,manifest['files'])

    def build_fixture(self, directory):
        root=Path(directory)/'source';root.mkdir()
        files={'cpp/src/Engine.cpp':'int engine;', 'cpp/include/graphdb/CGraphDB.h':'void graphdb_close(void);',
          'cpp/include/graphdb/Engine.hpp':'header', 'cpp/third_party/nlohmann/json.hpp':'third party',
          'cpp/third_party/nlohmann/LICENSE.MIT':'MIT', 'swift/Sources/GraphDBKit/Graph.swift':'wrapper',
          'LICENSE':'END OF TERMS AND CONDITIONS', 'tools/apple_artifacts.py':'builder',
          'licenses/dependencies.json':'[]', 'devtools/Package.resolved':'{"pins":[]}',
          'docs-web/package-lock.json':'{"packages":{}}'}
        for name,contents in files.items():
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(contents)
        return root,files

    def fake_build(self, root, output, *, dirty=False, mutation=None):
        state={'revision':'a'*40,'sourceTree':'b'*40,'dirty':dirty};events=[];mutated=False
        def command(args, **kwargs):
            nonlocal mutated
            args=list(map(str,args))
            if args[0]=='git':
                events.append(('git',args[1:]))
                if args[1]=='status':return ' M cpp/src/Engine.cpp' if state['dirty'] else ''
                if args[-1]=='HEAD':return state['revision']
                if args[-1]=='HEAD^{tree}':return state['sourceTree']
                raise AssertionError(args)
            if '-c' in args:
                events.append(('compile',args))
                Path(args[args.index('-o')+1]).write_bytes(b'object')
                if mutation and not mutated:mutation(state);mutated=True
            elif args[:2]==['xcrun','libtool']:
                Path(args[args.index('-o')+1]).write_bytes(b'archive')
            elif args[:2]==['xcodebuild','-create-xcframework']:
                framework=Path(args[args.index('-output')+1]);framework.mkdir()
                (framework/'Info.plist').write_bytes(plistlib.dumps({'AvailableLibraries':[]}))
            elif args[:2]==['xcrun','--find']:return '/fixture/clang++'
            elif args[0] in ('xcodebuild','swift','/fixture/clang++'):return 'fixture toolchain'
            else:raise AssertionError(args)
            return ''
        row=next(row for row in self.api.SLICES if row['id']=='ios')
        sdk={'iphoneos':{'path':'/fixture/sdk','version':'16.0','build':'fixture'}}
        args=argparse.Namespace(version='1.2.3',output=str(output),allow_dirty=dirty,xcode_build=None,jobs=1)
        with patch.object(self.api,'ROOT',root),patch.object(self.api,'SLICES',[row]), \
             patch.object(self.api,'run',side_effect=command),patch.object(self.api,'sdk_inventory',return_value=sdk), \
             patch.object(self.api,'inspect_library'),patch.object(self.api,'verify_bundle'), \
             patch('sys.stdout',new_callable=io.StringIO):
            self.api.build(args)
        return json.loads((output/'NovaGraph-1.2.3/artifact-manifest.json').read_text()),events

    def test_build_rejects_source_edits_before_publishing(self):
        for dirty in (False,True):
            with self.subTest(dirty=dirty),tempfile.TemporaryDirectory() as directory:
                root,_=self.build_fixture(directory);output=Path(directory)/'candidate'
                def edit(state):
                    (root/'cpp/src/Engine.cpp').write_text('changed source')
                    state['dirty']=True
                with self.assertRaisesRegex(ValueError,'source.*changed|changed.*source'):
                    self.fake_build(root,output,dirty=dirty,mutation=edit)
                self.assertFalse(output.exists())

    def test_build_rejects_git_identity_drift_before_publishing(self):
        for field in ('revision','sourceTree','dirty'):
            with self.subTest(field=field),tempfile.TemporaryDirectory() as directory:
                root,_=self.build_fixture(directory);output=Path(directory)/'candidate'
                with self.assertRaisesRegex(ValueError,'source.*changed|changed.*source'):
                    self.fake_build(root,output,mutation=lambda state:state.update({field:True if field=='dirty' else 'c'*40}))
                self.assertFalse(output.exists())

    def test_build_records_starting_identity_and_allows_stable_dirty_inputs(self):
        for dirty in (False,True):
            with self.subTest(dirty=dirty),tempfile.TemporaryDirectory() as directory:
                root,_=self.build_fixture(directory);output=Path(directory)/'candidate'
                manifest,events=self.fake_build(root,output,dirty=dirty)
                self.assertEqual(manifest['revision'],'a'*40)
                self.assertEqual(manifest['sourceTree'],'b'*40)
                self.assertEqual(manifest['dirty'],dirty)
                self.assertRegex(manifest.get('sourceDigest',''),'^[0-9a-f]{64}$')
                before_compile=[event for event in events[:next(i for i,e in enumerate(events) if e[0]=='compile')]]
                self.assertIn(('git',['rev-parse','HEAD']),before_compile)
                self.assertIn(('git',['rev-parse','HEAD^{tree}']),before_compile)

    def test_build_input_digest_covers_all_distributed_inputs(self):
        self.assertTrue(callable(getattr(self.api,'build_input_digest',None)),'build input digest is missing')
        with tempfile.TemporaryDirectory() as directory:
            root,files=self.build_fixture(directory)
            with patch.object(self.api,'ROOT',root):
                original=self.api.build_input_digest()
                for name,contents in files.items():
                    with self.subTest(name=name):
                        path=root/name;path.write_text(contents+'changed')
                        self.assertNotEqual(original,self.api.build_input_digest())
                        path.write_text(contents)
                extra=root/'cpp/src/New.cpp';extra.write_text('new translation unit')
                self.assertNotEqual(original,self.api.build_input_digest());extra.unlink()
                removed=root/'swift/Sources/GraphDBKit/Graph.swift';removed.unlink()
                self.assertNotEqual(original,self.api.build_input_digest())

if __name__=='__main__': unittest.main()
