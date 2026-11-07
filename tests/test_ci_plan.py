import copy
import importlib.util
import json
import hashlib
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ci_plan import ALL_PLATFORMS, make_plan, release_platforms, require_successful_jobs
from release_downloads import download_buttons


class BuildPlanTests(unittest.TestCase):
    commit = 'a' * 40

    def test_daily_build_and_release_require_windows(self):
        self.assertEqual(release_platforms(make_plan(self.commit), self.commit), {'windows-x64'})

    def test_manual_full_build_keeps_all_four_platforms(self):
        self.assertEqual(release_platforms(make_plan(self.commit, True), self.commit), ALL_PLATFORMS)

    def test_cli_emits_matching_matrix_and_provenance(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'plan.json'
            output = subprocess.check_output([sys.executable, ROOT/'scripts/ci_plan.py', '--commit', self.commit,
                                              '--all-platforms', 'true', '--output', path], text=True)
            matrix = json.loads(output.removeprefix('matrix='))
            self.assertEqual({row['name'] for row in matrix['include']},
                             release_platforms(json.loads(path.read_text()), self.commit))

    def test_plan_cannot_hide_a_missing_or_duplicate_platform(self):
        for platforms in ([], ['linux-x64'], ['windows-x64', 'windows-x64'],
                          ['windows-x64', 'macos-x64'], ['windows-x64', 'unknown'], 'windows-x64', [None]):
            with self.subTest(platforms=platforms), self.assertRaises(ValueError):
                release_platforms(dict(schema=1, commit=self.commit, platforms=platforms), self.commit)

    def test_foreign_source_and_unknown_schema_are_rejected(self):
        for changes in (dict(commit='b'*40), dict(schema=2)):
            plan = make_plan(self.commit); plan.update(changes)
            with self.assertRaises(ValueError): release_platforms(plan, self.commit)
        for commit in ('main', '', 'a'*7):
            with self.assertRaises(ValueError): make_plan(commit)

    def test_windows_release_only_links_available_verified_windows(self):
        names = ['UmekoCpp-v0.7.1-windows-x64.zip']
        block = download_buttons('umeiko/umeko-desktop-cpp', 'v0.7.1', names, platforms={'windows-x64'})
        self.assertEqual(block.count('/releases/download/'), 1)
        self.assertIn(names[0], block)
        self.assertNotIn('macos', block)
        # An explicit plan never makes a missing package optional.
        with self.assertRaises(ValueError):
            download_buttons('umeiko/umeko-desktop-cpp', 'v0.7.1', [], platforms={'windows-x64'})
        # Existing callers still require all four downloads.
        with self.assertRaises(ValueError): download_buttons('umeiko/umeko-desktop-cpp', 'v0.7.1', names)

    def test_invalid_download_platforms_fail_closed(self):
        for platforms in (set(), {'unknown'}):
            with self.assertRaises(ValueError):
                download_buttons('umeiko/umeko-desktop-cpp', 'v0.7.1', [], platforms=platforms)

    def test_rerun_uses_latest_job_and_still_requires_shared_checks(self):
        jobs = [dict(id=1, name='windows-x64', conclusion='failure'),
                dict(id=2, name='windows-x64', conclusion='success'),
                dict(id=3, name='plan', conclusion='success'),
                dict(id=4, name='Shared runtime and release checks', conclusion='success')]
        require_successful_jobs(jobs, {'windows-x64'}, shared=True)
        with self.assertRaises(ValueError): require_successful_jobs(jobs[:-1], {'windows-x64'}, shared=True)
        jobs.append(dict(id=5, name='windows-x64', conclusion='failure'))
        with self.assertRaises(ValueError): require_successful_jobs(jobs, {'windows-x64'}, shared=True)


class ReleaseInputsValidated(Exception):
    pass


class ReleaseInputTests(unittest.TestCase):
    """Exercise the real publisher up to its first possible remote mutation."""
    commit = 'a'*40

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.folder = Path(self.temp.name)
        self.source = self.folder/'fixtures'; self.source.mkdir()
        self.plan = make_plan(self.commit)
        self.run = dict(status='completed', conclusion='success', head_branch='main', head_repository={'full_name':'umeiko/umeko-desktop-cpp'},
                        event='push', path='.github/workflows/build.yml', head_sha=self.commit)
        self.bad_job = None
        self.old = Path.cwd(); os.chdir(self.folder)
        self.populate({'windows-x64'})

    def tearDown(self):
        os.chdir(self.old); self.temp.cleanup()

    def populate(self, platforms):
        for target in platforms:
            suffix = '.tar.gz' if target == 'linux-x64' else '.zip'
            archive = self.source/('UmekoCpp-v0.7.1-'+target+suffix)
            body = ('fixture '+target).encode(); archive.write_bytes(body)
            digest = hashlib.sha256(body).hexdigest()
            archive.with_name(archive.name+'.sha256').write_text(digest+'  '+archive.name+'\n')
            report = dict(platform=target, commit=self.commit, version='0.7.1', edition='standard',
                          bundlePython=True, nodeDistribution={'version':'22.23.3'}, bytes=len(body), sha256=digest)
            if target.startswith('macos-'):
                disk = archive.with_suffix('.dmg'); disk.write_bytes(body)
                disk.with_name(disk.name+'.sha256').write_text(digest+'  '+disk.name+'\n')
                report['diskImage'] = dict(name=disk.name, bytes=len(body), sha256=digest)
            (self.source/('UmekoCpp-'+target+'.json')).write_text(json.dumps(report))
        for name in ['umeko-provider-sdk-0.2.2.zip', 'develop-umeko-provider.zip']:
            (self.source/name).write_bytes(b'fixture sdk')

    def gh(self, argv, **kwargs):
        self.assertEqual(argv[0], 'gh')
        args = argv[1:]
        if args[0] == 'api':
            path = args[1].split('?', 1)[0]
            if path.endswith('/artifacts'):
                return json.dumps({'artifacts': [] if self.plan is None else [dict(name='ci-build-plan', expired=False)]})
            if path.endswith('/actions/runs/123'): return json.dumps(self.run)
            if path.endswith('/jobs'):
                names = list(ALL_PLATFORMS if self.plan is None else self.plan['platforms'])
                if self.plan is not None: names += ['plan', 'Shared runtime and release checks']
                jobs = [dict(id=i+1, name=name, conclusion='success') for i,name in enumerate(names)]
                if self.bad_job: jobs.append(dict(id=100, name=self.bad_job, conclusion='skipped'))
                return json.dumps({'jobs': jobs})
            if path.endswith('/releases'): raise ReleaseInputsValidated()
        if args[:2] == ['run', 'download']:
            destination = Path(args[args.index('--dir')+1]); destination.mkdir(parents=True, exist_ok=True)
            if '--name' in args:
                (destination/'build-plan.json').write_text(json.dumps(self.plan))
            else:
                for source in self.source.iterdir(): shutil.copy2(source, destination/source.name)
            return ''
        self.fail('Unexpected remote operation: '+str(args[:3]))

    def verify(self):
        with patch.dict(os.environ, {'GITHUB_REPOSITORY':'umeiko/umeko-desktop-cpp',
                                     'RELEASE_BUILD_RUN':'123', 'RELEASE_TAG':'v0.7.1'}), \
             patch('subprocess.check_output', self.gh), \
             patch('subprocess.run', side_effect=AssertionError('Publication is forbidden in fixtures')):
            runpy.run_path(str(ROOT/'scripts/publish-release.py'), run_name='__main__')

    def test_windows_plan_verifies_exactly_six_assets_before_remote_changes(self):
        with self.assertRaises(ReleaseInputsValidated): self.verify()
        files = list((self.folder/'.release/publish').iterdir())
        self.assertEqual(len(files), 6)
        sums = (self.folder/'.release/publish/SHA256SUMS.txt').read_text().splitlines()
        self.assertEqual(len(sums), 5)
        for line in sums:
            digest, name = line.split()
            self.assertEqual(digest, hashlib.sha256((self.folder/'.release/publish'/name).read_bytes()).hexdigest())

    def test_legacy_run_still_requires_four_platforms(self):
        self.plan = None
        with self.assertRaises(AssertionError): self.verify()
        self.populate(ALL_PLATFORMS)
        with self.assertRaises(ReleaseInputsValidated): self.verify()
        self.assertEqual(len(list((self.folder/'.release/publish').iterdir())), 19)

    def test_full_plan_cannot_silently_publish_only_windows(self):
        self.plan = make_plan(self.commit, True)
        with self.assertRaises(AssertionError): self.verify()

    def test_foreign_commit_plan_is_rejected(self):
        self.plan = make_plan('b'*40)
        with self.assertRaises(ValueError): self.verify()

    def test_foreign_manifest_and_corrupt_archive_are_rejected(self):
        path = self.source/'UmekoCpp-windows-x64.json'
        original = path.read_text(); report = json.loads(original); report['commit'] = 'b'*40
        path.write_text(json.dumps(report))
        with self.assertRaises(AssertionError): self.verify()
        path.write_text(original)
        (self.source/'UmekoCpp-v0.7.1-windows-x64.zip').write_bytes(b'corrupt')
        with self.assertRaises(AssertionError): self.verify()

    def test_failed_or_untrusted_run_is_never_published(self):
        for changes in [dict(conclusion='failure'), dict(head_branch='feature'), dict(event='pull_request'),
                        dict(path='.github/workflows/other.yml'), dict(head_repository={'full_name':'someone/fork'})]:
            original = copy.deepcopy(self.run); self.run.update(changes)
            with self.subTest(changes=changes), self.assertRaises(AssertionError): self.verify()
            self.run = original

    def test_green_run_with_missing_or_skipped_required_tests_is_rejected(self):
        for name in ['windows-x64', 'Shared runtime and release checks', 'plan']:
            self.bad_job = name
            with self.subTest(job=name), self.assertRaisesRegex(ValueError, 'Required CI job'):
                self.verify()


if __name__ == '__main__': unittest.main()
