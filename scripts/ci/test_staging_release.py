"""Release admission, immutable artifacts, migration, and production compatibility."""

import base64
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

import staging_dispatch as dispatch
import staging_manifest as manifest
import staging_source

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github' / 'workflows'
REPOSITORY = 'example/opennova'
SHA = 'a' * 40
RUN_ID = '1234'


def ci_run(**updates):
    return {
        'id': int(RUN_ID), 'run_attempt': 1, 'run_number': 20,
        'repository': {'full_name': REPOSITORY},
        'head_repository': {'full_name': REPOSITORY},
        'path': '.github/workflows/ci.yml', 'event': 'push',
        'head_branch': 'staging', 'head_sha': SHA,
        'run_started_at': '2026-10-29T10:00:00Z',
        'status': 'in_progress', 'conclusion': None, **updates,
    }


def artifact(name, artifact_id=1, **updates):
    return {
        'id': artifact_id, 'name': name, 'expired': False,
        'created_at': '2026-10-29T10:05:00Z', 'digest': 'sha256:' + 'b' * 64,
        'workflow_run': {'id': int(RUN_ID), 'head_sha': SHA}, **updates,
    }


def release():
    return {
        'schema': 1, **manifest.identity(REPOSITORY, ci_run()),
        'images': {key: f'ghcr.io/example/{repo}@sha256:' + 'c' * 64
                   for key, repo in manifest.IMAGE_REPOSITORIES.items()},
        'packages': {key: {'id': index + 10, 'name': name, 'sha256': 'b' * 64}
                     for index, (key, name) in enumerate([
                         ('runtime', 'opennova-runtime-windows-v1.2.3.zip'),
                         ('editor', 'opennova-editor-windows-v1.2.3.zip'),
                         ('tools', 'opennova-tools-windows.zip'),
                         ('blender', 'opennova-blender-addon-windows.zip')])},
    }


def workflow(name):
    return (WORKFLOWS / f'{name}.yml').read_text(encoding='utf-8')


def job(name, job_id):
    jobs = workflow(name).split('\njobs:\n', 1)[1]
    match = re.search(r'^  ' + re.escape(job_id) + r':\n(.*?)(?=^  [\w-]+:|\Z)', jobs, re.M | re.S)
    if match is None:
        raise AssertionError(f'no {job_id} job in {name}')
    return match[1]


def needs(body):
    value = re.search(r'^    needs: (.+)$', body, re.M).group(1)
    return [item.strip() for item in value.strip('[]').split(',')]


class StagingSource(unittest.TestCase):
    def check(self, run, ref='refs/heads/staging', attempt='1'):
        staging_source.validate(run, REPOSITORY, RUN_ID, SHA, ref, attempt)

    def test_only_staging_pushes_and_manual_ci_are_admitted(self):
        for event in ('push', 'workflow_dispatch'):
            self.check(ci_run(event=event))

    def test_prs_forks_tags_other_workflows_wrong_sha_and_attempt_are_rejected(self):
        cases = [
            {'event': 'pull_request'}, {'event': 'pull_request_target'},
            {'event': 'workflow_run'}, {'event': 'release'}, {'head_branch': 'master'},
            {'id': 1235}, {'head_sha': 'b' * 40}, {'run_attempt': 2},
            {'repository': {'full_name': 'another/opennova'}},
            {'head_repository': {'full_name': 'fork/opennova'}}, {'head_repository': None},
            {'path': '.github/workflows/release.yml'},
        ]
        for update in cases:
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.check(ci_run(**update))
        for ref in ('refs/pull/9/merge', 'refs/heads/master', 'refs/tags/staging'):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                self.check(ci_run(), ref)

    def test_unrelated_failure_or_parent_cancellation_does_not_revoke_ready_components(self):
        for conclusion in ('failure', 'cancelled'):
            self.check(ci_run(status='completed', conclusion=conclusion))

    def test_source_checks_current_and_named_attempt(self):
        with patch.object(manifest, 'gh_api', side_effect=[ci_run(), ci_run()]) as api:
            self.assertEqual(manifest.source(REPOSITORY, RUN_ID, '1', SHA), ci_run())
            self.assertEqual(api.call_args_list[-1].args[0],
                             f'repos/{REPOSITORY}/actions/runs/{RUN_ID}/attempts/1')
        with patch.object(manifest, 'gh_api', return_value=ci_run(run_attempt=2)):
            with self.assertRaises(ValueError):
                manifest.source(REPOSITORY, RUN_ID, '1', SHA, require_current_attempt=True)

    def test_admitted_source_attempt_survives_a_newer_ci_attempt(self):
        admitted = ci_run(status='completed', conclusion='cancelled')
        with patch.object(manifest, 'gh_api', side_effect=[ci_run(run_attempt=2), admitted]):
            self.assertEqual(manifest.source(REPOSITORY, RUN_ID, '1', SHA), admitted)
        with patch.object(manifest, 'gh_api', return_value=ci_run(run_attempt=1)) as api:
            with self.assertRaisesRegex(ValueError, 'does not exist'):
                manifest.source(REPOSITORY, RUN_ID, '2', SHA)
            self.assertEqual(api.call_count, 1)
        with patch.object(manifest, 'gh_api', side_effect=[ci_run(run_attempt=2), ci_run(run_attempt=2)]):
            with self.assertRaises(ValueError):
                manifest.source(REPOSITORY, RUN_ID, '1', SHA)

    def test_only_admission_rechecks_latest_head(self):
        argv = ['read', '--repository', REPOSITORY, '--run-id', RUN_ID, '--attempt', '1',
                '--sha', SHA, '--manifest-id', '99']
        for latest in (False, True):
            output = io.StringIO()
            with (patch.object(manifest, 'source', return_value=ci_run(conclusion='cancelled')),
                  patch.object(manifest, 'load', return_value=release()),
                  patch.object(manifest, 'gh_api', return_value={'object': {'sha': 'd' * 40}}) as api,
                  contextlib.redirect_stdout(output)):
                self.assertEqual(manifest.main(argv + (['--latest'] if latest else [])), 0)
                self.assertEqual(json.loads(output.getvalue())['current'], not latest)
                self.assertEqual(api.call_count, int(latest))


class ManifestIntegrity(unittest.TestCase):
    def test_manifest_requires_every_image_and_package_from_the_expected_source(self):
        manifest.validate_manifest(release(), REPOSITORY, ci_run())
        variants = []
        for field in ('images', 'packages'):
            changed = release()
            changed[field].pop(next(iter(changed[field])))
            variants.append(changed)
        for key, value in [('run_id', 999), ('run_attempt', 2), ('sha', 'd' * 40)]:
            changed = release()
            changed['source'][key] = value
            variants.append(changed)
        changed = release()
        changed['images']['serve'] = 'ghcr.io/example/opennova-serve:latest'
        variants.append(changed)
        changed = release()
        changed['images']['serve'] = 'ghcr.io/outsider/opennova-serve@sha256:' + 'c' * 64
        variants.append(changed)
        changed = release()
        changed['packages']['runtime']['name'] = '../runtime.zip'
        variants.append(changed)
        changed = release()
        changed['packages']['editor']['id'] = changed['packages']['runtime']['id']
        variants.append(changed)
        for changed in variants:
            with self.subTest(manifest=changed), self.assertRaises(ValueError):
                manifest.validate_manifest(changed, REPOSITORY, ci_run())

    def test_expired_cross_run_old_attempt_and_unhashed_artifacts_are_rejected(self):
        for update in (
            {'expired': True}, {'workflow_run': {'id': 999, 'head_sha': SHA}},
            {'workflow_run': {'id': int(RUN_ID), 'head_sha': 'f' * 40}},
            {'created_at': '2026-10-29T09:59:59Z'}, {'digest': None},
        ):
            with self.subTest(update=update), self.assertRaises(ValueError):
                manifest.artifact_ok(artifact('test', **update), ci_run())

    def test_old_attempt_error_explains_how_to_rebuild_without_replacing_admitted_ids(self):
        with self.assertRaisesRegex(ValueError, 'start a new manual staging CI run'):
            manifest.artifact_ok(artifact('test', created_at='2026-10-29T09:59:59Z'), ci_run())

    def test_create_freezes_ten_digest_records_and_four_artifact_ids(self):
        expected = release()
        artifacts = [artifact(f'image-digest-{key}-1', index + 100)
                     for index, key in enumerate(manifest.IMAGE_REPOSITORIES)]
        artifacts += [artifact(item['name'], item['id']) for item in expected['packages'].values()]

        def image_record(repository, item, filename):
            key = filename.removesuffix('.json')
            repository, digest = expected['images'][key].split('@')
            return {'image': key, 'repository': repository, 'digest': digest,
                    'source_sha': SHA, 'run_id': int(RUN_ID), 'run_attempt': 1}

        with (patch.object(manifest, 'pages', return_value=artifacts),
              patch.object(manifest, 'artifact_json', side_effect=image_record)):
            self.assertEqual(manifest.create(REPOSITORY, ci_run()), expected)
        for altered in (artifacts[:-1], artifacts + [artifacts[0]]):
            with (patch.object(manifest, 'pages', return_value=altered),
                  patch.object(manifest, 'artifact_json', side_effect=image_record),
                  self.assertRaises(ValueError)):
                manifest.create(REPOSITORY, ci_run())

    def test_manifest_artifact_id_name_and_archive_checksum_are_checked(self):
        item = artifact('staging-release-1', 99)
        ready = [{'name': manifest.READINESS_JOB, 'status': 'completed', 'conclusion': 'success'}]
        with (patch.object(manifest, 'pages', return_value=ready),
              patch.object(manifest, 'gh_api', return_value=item),
              patch.object(manifest, 'artifact_json', return_value=release())):
            self.assertEqual(manifest.load(REPOSITORY, ci_run(), '99'), release())
        for update in ({'id': 98}, {'name': 'staging-release-2'}):
            with (patch.object(manifest, 'pages', return_value=ready),
                  patch.object(manifest, 'gh_api', return_value=item | update),
                  self.assertRaises(ValueError)):
                manifest.load(REPOSITORY, ci_run(), '99')
        with patch.object(manifest.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'changed')):
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                manifest.artifact_bytes(REPOSITORY, item)

    def test_only_successful_readiness_in_the_named_attempt_admits_a_manifest(self):
        for conclusion in ('failure', 'cancelled', 'skipped', None):
            jobs = [{'name': manifest.READINESS_JOB, 'status': 'completed', 'conclusion': conclusion}]
            with (patch.object(manifest, 'pages', return_value=jobs) as pages,
                  patch.object(manifest, 'gh_api') as api, self.assertRaises(ValueError)):
                manifest.load(REPOSITORY, ci_run(), '99')
            api.assert_not_called()
            self.assertIn('/attempts/1/jobs', pages.call_args.args[0])
        with patch.object(manifest, 'pages', return_value=[]), self.assertRaises(ValueError):
            manifest.load(REPOSITORY, ci_run(), '99')

    def test_archive_is_not_extracted_and_rejects_unexpected_paths(self):
        for filename in ('../release.json', 'other.json'):
            data = io.BytesIO()
            with zipfile.ZipFile(data, 'w') as archive:
                archive.writestr(filename, '{}')
            with (patch.object(manifest, 'artifact_bytes', return_value=data.getvalue()),
                  self.assertRaises(ValueError)):
                manifest.artifact_json(REPOSITORY, artifact('release'), 'release.json')

    def test_downloads_exact_package_ids_and_verifies_bytes_before_publish(self):
        frozen = release()
        data = b'package zip bytes'
        digest = hashlib.sha256(data).hexdigest()
        for package in frozen['packages'].values():
            package['sha256'] = digest
        replies = [artifact(p['name'], p['id'], digest='sha256:' + digest)
                   for p in frozen['packages'].values()]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            with (patch.object(manifest, 'gh_api', side_effect=replies) as api,
                  patch.object(manifest.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, data))):
                manifest.download_packages(REPOSITORY, ci_run(), frozen, destination)
                self.assertEqual(len(list(destination.iterdir())), 4)
                self.assertTrue(all(path.read_bytes() == data for path in destination.iterdir()))
                self.assertEqual([call.args[0].rsplit('/', 1)[1] for call in api.call_args_list],
                                 ['10', '11', '12', '13'])
        with tempfile.TemporaryDirectory() as directory:
            with (patch.object(manifest, 'gh_api', return_value=replies[0]),
                  patch.object(manifest.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'tampered')),
                  self.assertRaisesRegex(ValueError, 'checksum mismatch')):
                manifest.download_packages(REPOSITORY, ci_run(), frozen, Path(directory))

    def test_artifact_and_run_lists_are_paginated(self):
        with patch.object(manifest, 'gh_api', side_effect=[{'items': [{}] * 100}, {'items': [{}]}]) as api:
            self.assertEqual(len(manifest.pages('endpoint?branch=staging', 'items')), 101)
            self.assertIn('&per_page=100&page=2', api.call_args_list[-1].args[0])


class StagingDispatch(unittest.TestCase):
    def test_cancellation_and_dispatch_cli_require_the_current_source_attempt(self):
        for command in ('cancel-older', 'dispatch'):
            with (patch.object(dispatch, 'source', return_value=ci_run()) as source,
                  patch.object(dispatch, 'cancel_older', return_value=[]),
                  patch.object(dispatch, 'dispatch', return_value=True),
                  contextlib.redirect_stdout(io.StringIO())):
                self.assertEqual(dispatch.main([
                    command, '--repository', REPOSITORY, '--run-id', RUN_ID,
                    '--attempt', '1', '--sha', SHA, '--manifest-id', '99',
                ]), 0)
                source.assert_called_once_with(REPOSITORY, RUN_ID, '1', SHA,
                                               require_current_attempt=True)

    def test_default_branch_registration_and_interface_are_required(self):
        content = {'content': base64.b64encode(workflow('staging-rollout').encode()).decode()}
        with patch.object(dispatch, 'gh_api', side_effect=[{'default_branch': 'master'}, content, {'state': 'active'}]):
            self.assertTrue(dispatch.available(REPOSITORY))
        old = {'content': base64.b64encode(b'on:\n  workflow_call:\n').decode()}
        with patch.object(dispatch, 'gh_api', side_effect=[{'default_branch': 'master'}, old]):
            self.assertFalse(dispatch.available(REPOSITORY))
        # The interface as #1296 shipped it, a workflow_call block after the dispatch inputs,
        # still reads: the default branch may carry it until staging next reaches it.
        both = workflow('staging-rollout').replace(
            '\n\nconcurrency:', '\n  workflow_call:\n    inputs:\n      run_id:\n        type: string\n'
            '\nconcurrency:', 1)
        both_content = {'content': base64.b64encode(both.encode()).decode()}
        with patch.object(dispatch, 'gh_api',
                          side_effect=[{'default_branch': 'master'}, both_content, {'state': 'active'}]):
            self.assertTrue(dispatch.available(REPOSITORY))
        # An input the dispatch sends that the default branch's interface lacks.
        short = workflow('staging-rollout').replace('      manifest_id:\n', '      manifest:\n', 1)
        short_content = {'content': base64.b64encode(short.encode()).decode()}
        with patch.object(dispatch, 'gh_api', side_effect=[{'default_branch': 'master'}, short_content]):
            self.assertFalse(dispatch.available(REPOSITORY))
        missing = subprocess.CalledProcessError(1, ['gh'], stderr='gh: Not Found (HTTP 404)')
        with patch.object(dispatch, 'gh_api', side_effect=[{'default_branch': 'master'}, missing]):
            self.assertFalse(dispatch.available(REPOSITORY))
        forbidden = subprocess.CalledProcessError(1, ['gh'], stderr='gh: Forbidden (HTTP 403)')
        with patch.object(dispatch, 'gh_api', side_effect=[{'default_branch': 'master'}, forbidden]):
            with self.assertRaises(subprocess.CalledProcessError):
                dispatch.available(REPOSITORY)

    def test_older_trusted_staging_builds_are_cancelled(self):
        oldest = ci_run(id=100, run_number=17)
        older = ci_run(id=101, run_number=18)
        newer = ci_run(id=102, run_number=21)
        fork = ci_run(id=103, run_number=19, head_repository={'full_name': 'fork/repo'})
        with (patch.object(dispatch, 'current', return_value=True),
              patch.object(dispatch, 'pages', return_value=[oldest, older, newer, fork]),
              patch.object(dispatch, 'gh_api', side_effect=[oldest, older, fork]),
              patch.object(dispatch, 'request') as request):
            self.assertEqual(dispatch.cancel_older(REPOSITORY, ci_run()), [100, 101])
            self.assertEqual([call.args[0] for call in request.call_args_list],
                             [f'repos/{REPOSITORY}/actions/runs/100/cancel',
                              f'repos/{REPOSITORY}/actions/runs/101/cancel'])

    def test_a_run_no_longer_staging_head_cancels_nothing(self):
        with (patch.object(dispatch, 'current', return_value=False),
              patch.object(dispatch, 'request') as request):
            self.assertEqual(dispatch.cancel_older(REPOSITORY, ci_run()), [])
            request.assert_not_called()

    def test_dispatch_passes_explicit_source_and_does_not_inherit_parent_lifetime(self):
        with (patch.object(dispatch, 'available', return_value=True),
              patch.object(dispatch, 'load', return_value=release()),
              patch.object(dispatch, 'current', return_value=True),
              patch.object(dispatch, 'request') as request):
            self.assertTrue(dispatch.dispatch(REPOSITORY, ci_run(), '99'))
            request.assert_called_once_with(
                f'repos/{REPOSITORY}/actions/workflows/staging-rollout.yml/dispatches',
                {'ref': 'staging', 'inputs': {'run_id': RUN_ID, 'source_attempt': '1',
                                            'source_sha': SHA, 'manifest_id': '99'}})

    def test_dispatch_refuses_while_the_default_branch_carries_no_dispatcher(self):
        with (patch.object(dispatch, 'available', return_value=False),
              patch.object(dispatch, 'request') as request,
              self.assertRaises(ValueError)):
            dispatch.dispatch(REPOSITORY, ci_run(), '99')
        request.assert_not_called()

    def test_the_cli_has_no_probe(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            dispatch.main(['probe', '--repository', REPOSITORY])


class StagingWorkflow(unittest.TestCase):
    def test_component_readiness_and_the_dispatched_rollout_are_explicit(self):
        ready = job('ci', 'staging-components-ready')
        self.assertIn('name: ' + manifest.READINESS_JOB, ready)
        self.assertCountEqual(needs(ready), [
            'service-images', 'serve-images', 'package-godot-windows',
            'package-blender-windows', 'package-apps-windows',
        ])
        self.assertNotRegex(ready, r'if:.*(?:always|failure|cancelled)\(')
        self.assertIn('staging_manifest.py create', ready)
        self.assertIn('name: staging-release-${{ github.run_attempt }}', ready)
        # One route: the release dispatched as a rollout run of its own, never one in the CI run.
        route = job('ci', 'staging-rollout')
        self.assertEqual(needs(route), ['staging-components-ready'])
        self.assertIn('staging_dispatch.py dispatch', route)
        self.assertNotIn('uses: ./.github/workflows/staging-rollout.yml', workflow('ci'))
        self.assertNotIn('staging-rollout-mode', workflow('ci'))
        events = workflow('staging-rollout').split('\non:\n', 1)[1].split('\nconcurrency:', 1)[0]
        self.assertIn('  workflow_dispatch:\n', events)
        self.assertNotIn('  workflow_call:\n', events)
        cancel = job('ci', 'staging-cancel-older')
        for body in (route, cancel):
            self.assertIn("github.ref == 'refs/heads/staging'", body)
            self.assertIn("github.event_name == 'push'", body)
            self.assertIn("github.event_name == 'workflow_dispatch'", body)
            self.assertIn('actions: write', body)
        self.assertIn('staging_dispatch.py cancel-older', cancel)
        self.assertNotRegex(cancel, re.compile(r'^    needs:', re.M))

    def test_lock_spans_latest_admission_deploy_and_publish(self):
        rollout = workflow('staging-rollout')
        self.assertIn('concurrency:\n  group: staging-rollout\n', rollout)
        self.assertIn('  queue: max\n  cancel-in-progress: false', rollout)
        self.assertIn('--latest', job('staging-rollout', 'source'))
        self.assertIn('"$GITHUB_REF" == refs/heads/staging', job('staging-rollout', 'source'))
        self.assertEqual(needs(job('staging-rollout', 'deploy')), ['source'])
        self.assertCountEqual(needs(job('staging-rollout', 'publish')), ['source', 'deploy'])
        self.assertIn("if: needs.source.outputs.current == 'true'", job('staging-rollout', 'deploy'))
        for name in ('deploy', 'publish'):
            self.assertNotIn('--latest', workflow(name))
            self.assertNotIn('group: staging-rollout', workflow(name))
            self.assertIn('staging_manifest.py', workflow(name))
            self.assertNotIn('"$STAGING_ATTEMPT" == "$GITHUB_RUN_ATTEMPT"', workflow(name))

    def test_prod_remains_manual_promotion_and_staging_uses_manifest(self):
        for name in ('deploy', 'publish'):
            inputs = workflow(name).split('  workflow_dispatch:\n', 1)[1].split('  workflow_call:', 1)[0]
            self.assertIn('options: [prod]', inputs)
        deploy = job('deploy', 'deploy')
        self.assertIn('--deployment prod app deploy --images-from - --yes', deploy)
        self.assertIn('--deployment staging app deploy --images-from - --keep-hosting', deploy)
        self.assertNotIn('SOURCE_TAG', deploy)
        publish = job('publish', 'publish')
        self.assertIn("if: inputs.deployment == 'prod'\n        uses: actions/download-artifact@v8", publish)
        self.assertIn('staging_manifest.py packages', publish)
        self.assertIn('"$workflow" == .github/workflows/release.yml', publish)
        self.assertIn('scripts/ci/one_stamp.sh "$head" "$(jq -r .run_number <<<"$run")"', publish)
        self.assertNotIn('config/version', publish)
        self.assertLess(publish.index('staging_manifest.py read'),
                        publish.index('scripts/ci/one_stamp.sh "$head"'))


@unittest.skipUnless(shutil.which('bash'), 'workflow shell integration requires bash')
class DeploymentCommand(unittest.TestCase):
    def run_deploy(self, **environment):
        body = job('deploy', 'deploy').split('      - name: app deploy\n', 1)[1]
        script = textwrap.dedent(body.split('      - name:', 1)[0].split('        run: |\n', 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            command = Path(directory) / 'deploy' / 'bin' / 'on-deploy'
            command.parent.mkdir(parents=True)
            command.write_text('#!/usr/bin/env bash\nprintf "deploy %s\\n" "$*"\ncat\n', encoding='utf-8')
            command.chmod(0o755)
            env = os.environ | {'IN_DEPLOYMENT': 'staging', 'IN_PROMOTED': '',
                                'SOURCE_IMAGES': '', 'IN_NO_HOSTING': 'false', **environment}
            return subprocess.run([shutil.which('bash'), '--noprofile', '--norc', '-c', script],
                                  cwd=directory, env=env, capture_output=True, text=True)

    def test_staging_uses_only_admitted_digests_and_preserves_hosting(self):
        pinned = json.dumps({'deployment': 'staging', 'release': 'ci-1234-1', 'images': release()['images']})
        result = self.run_deploy(SOURCE_IMAGES=pinned)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'deploy --deployment staging app deploy --images-from - --keep-hosting\n' + pinned)
        result = self.run_deploy()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('deploy --deployment', result.stdout)

    def test_prod_promotes_recorded_release_and_honors_hosting_choice(self):
        pinned = '{"deployment":"staging","release":"previous","images":{}}'
        result = self.run_deploy(IN_DEPLOYMENT='prod', IN_PROMOTED=pinned, IN_NO_HOSTING='true')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'deploy --deployment prod app deploy --images-from - --yes --no-hosting\n' + pinned)
        result = self.run_deploy(IN_DEPLOYMENT='prod')
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
