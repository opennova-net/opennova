"""Staging source admission and the workflow edges that can change a deployment."""

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import staging_source

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github' / 'workflows'
REPOSITORY = 'example/opennova'
SHA = 'a' * 40
RUN_ID = '1234'


def ci_run(**updates):
    return {
        'id': int(RUN_ID),
        'repository': {'full_name': REPOSITORY},
        'head_repository': {'full_name': REPOSITORY},
        'path': '.github/workflows/ci.yml',
        'event': 'push',
        'head_branch': 'staging',
        'head_sha': SHA,
        'status': 'in_progress',
        'conclusion': None,
        **updates,
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


def expression_value(expression, *, event='push', ref='refs/heads/staging'):
    # Evaluate the checked-in selection expressions over an event/ref table, with
    # GitHub's boolean operators translated to Python. This is trusted repo code.
    expression = expression.strip().removeprefix('${{').removesuffix('}}').strip()
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    return eval(
        expression,
        {'__builtins__': {}},
        {
            'github': SimpleNamespace(event_name=event, ref=ref),
            'fromJSON': json.loads,
        },
    )


class StagingSource(unittest.TestCase):
    def check(self, run, ref='refs/heads/staging'):
        staging_source.validate(run, REPOSITORY, RUN_ID, SHA, ref)

    def test_only_staging_pushes_and_manual_ci_are_admitted(self):
        for event in ('push', 'workflow_dispatch'):
            with self.subTest(event=event):
                self.check(ci_run(event=event))

    def test_prs_from_or_into_staging_and_other_events_are_rejected(self):
        for event in ('pull_request', 'pull_request_target', 'workflow_run', 'release'):
            with self.subTest(event=event), self.assertRaises(ValueError):
                self.check(ci_run(event=event))
        for ref in (
            'refs/pull/9/merge',
            'refs/heads/master',
            'refs/heads/fix/a',
            'refs/tags/staging',
            'refs/tags/v1.0',
        ):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                self.check(ci_run(), ref)
        for branch in ('master', 'fix/a', None):
            with self.subTest(branch=branch), self.assertRaises(ValueError):
                self.check(ci_run(head_branch=branch))

    def test_run_commit_repository_and_workflow_must_match(self):
        cases = [
            {'id': 1235},
            {'head_sha': 'b' * 40},
            {'repository': {'full_name': 'another/opennova'}},
            {'head_repository': {'full_name': 'fork/opennova'}},
            {'head_repository': None},
            *[
                {'path': f'.github/workflows/{name}.yml'}
                for name in ('release', 'deploy', 'publish', 'novaworld-images', 'serve-image')
            ],
        ]
        for update in cases:
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.check(ci_run(**update))

    def test_unrelated_failure_does_not_reject_a_component_checked_run(self):
        # CI continues alongside rollout; its overall conclusion is not the gate.
        self.check(ci_run(status='completed', conclusion='failure'))

    def run_cli(self, replies, latest=True):
        stdout, stderr = io.StringIO(), io.StringIO()
        args = [
            '--repository',
            REPOSITORY,
            '--run-id',
            RUN_ID,
            '--sha',
            SHA,
            '--ref',
            'refs/heads/staging',
        ]
        if latest:
            args.append('--latest')
        with (
            patch.object(staging_source, 'gh_api', side_effect=replies) as api,
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            status = staging_source.main(args)
        return status, stdout.getvalue(), stderr.getvalue(), api.call_args_list

    def test_head_is_checked_after_source_validation(self):
        status, output, _, calls = self.run_cli(
            [
                ci_run(),
                {'object': {'sha': SHA}},
            ]
        )
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output), {'sha': SHA, 'tag': 'sha-aaaaaaa', 'current': True})
        self.assertEqual(
            [call.args[0] for call in calls],
            [
                f'repos/{REPOSITORY}/actions/runs/{RUN_ID}',
                f'repos/{REPOSITORY}/git/ref/heads/staging',
            ],
        )

    def test_superseded_run_skips_without_failing(self):
        status, output, _, _ = self.run_cli([ci_run(), {'object': {'sha': 'b' * 40}}])
        self.assertEqual(status, 0)
        self.assertFalse(json.loads(output)['current'])

    def test_active_rollout_can_finish_when_the_branch_advances(self):
        status, output, _, calls = self.run_cli([ci_run()], latest=False)
        self.assertEqual(status, 0)
        self.assertTrue(json.loads(output)['current'])
        self.assertEqual(
            len(calls), 1, 'entry points validate source without abandoning an active roll'
        )

    def test_missing_head_or_api_failure_stops_rollout(self):
        for reply in ({}, {'object': {'sha': None}}, subprocess.CalledProcessError(1, ['gh'])):
            with self.subTest(reply=reply):
                status, output, error, _ = self.run_cli([ci_run(), reply])
                self.assertEqual(status, 1)
                self.assertEqual(output, '')
                self.assertTrue(error.startswith('staging_source:'))


class StagingWorkflow(unittest.TestCase):
    def test_branch_builds_use_the_right_export_and_native_flavours(self):
        ci = workflow('ci')
        self.assertRegex(ci, r'push:\s*branches: \[master, staging\]')
        export = re.search(r'-ExportMode (\$\{\{.*?\}\})', ci).group(1)
        matrices = re.findall(r'target: (\$\{\{.*?\}\})', ci)
        self.assertEqual(len(matrices), 2)
        for event, ref, mode in [
            ('push', 'refs/heads/staging', 'debug'),
            ('workflow_dispatch', 'refs/heads/staging', 'debug'),
            ('pull_request', 'refs/pull/4/merge', 'debug'),
            ('push', 'refs/heads/master', 'release'),
            ('workflow_dispatch', 'refs/heads/feature', 'release'),
            ('push', 'refs/tags/v1.0', 'release'),
        ]:
            with self.subTest(event=event, ref=ref):
                self.assertEqual(expression_value(export, event=event, ref=ref), mode)
                want = (
                    ['template_debug']
                    if mode == 'debug'
                    else ['template_debug', 'template_release']
                )
                for matrix in matrices:
                    self.assertEqual(expression_value(matrix, event=event, ref=ref), want)

    def test_only_staging_branch_ci_calls_the_release_jobs(self):
        for job_id in ('staging-novaworld-images', 'staging-serve-images', 'staging-rollout'):
            body = job('ci', job_id)
            condition = re.search(r'^    if: (.+)$', body, re.M).group(1)
            for event, ref, admitted in [
                ('push', 'refs/heads/staging', True),
                ('workflow_dispatch', 'refs/heads/staging', True),
                ('pull_request', 'refs/pull/4/merge', False),
                ('pull_request', 'refs/heads/staging', False),
                ('push', 'refs/heads/master', False),
                ('workflow_dispatch', 'refs/heads/feature', False),
                ('push', 'refs/tags/staging', False),
            ]:
                with self.subTest(job=job_id, event=event, ref=ref):
                    self.assertEqual(
                        bool(expression_value(condition, event=event, ref=ref)), admitted
                    )
        for name in ('novaworld-images', 'serve-image'):
            self.assertIn('\n  workflow_call:\n', workflow(name))
        self.assertNotIn('publish-staging:', workflow('ci'))

    def test_all_components_gate_rollout_but_the_full_suite_does_not(self):
        dependencies = needs(job('ci', 'staging-rollout'))
        self.assertCountEqual(
            dependencies,
            [
                'staging-novaworld-images',
                'staging-serve-images',
                'package-godot-windows',
                'package-blender-windows',
                'package-apps-windows',
            ],
        )
        # No status-function override may bypass GitHub's default success() gate.
        for workflow_name, job_id in [('ci', 'staging-rollout'), ('staging-rollout', 'publish')]:
            self.assertNotRegex(job(workflow_name, job_id), r'if:.*(?:always|failure|cancelled)\(')
        self.assertCountEqual(needs(job('novaworld-images', 'push')), ['http-tests', 'build'])
        self.assertCountEqual(needs(job('serve-image', 'push')), ['http-tests', 'serve-image'])

    def test_one_lock_spans_source_deployment_and_publish(self):
        rollout = workflow('staging-rollout')
        self.assertIn('concurrency:\n  group: staging-rollout\n', rollout)
        self.assertIn('  queue: max\n  cancel-in-progress: false', rollout)
        self.assertIn('--latest', job('staging-rollout', 'source'))
        self.assertEqual(needs(job('staging-rollout', 'deploy')), ['source'])
        self.assertIn(
            "if: needs.source.outputs.current == 'true'", job('staging-rollout', 'deploy')
        )
        self.assertCountEqual(needs(job('staging-rollout', 'publish')), ['source', 'deploy'])
        for name in ('deploy', 'publish'):
            self.assertNotIn('group: staging-rollout', workflow(name))
            self.assertIn('staging_source.py', workflow(name))
            self.assertIn(
                'staging_attempt: ${{ needs.source.outputs.attempt }}', job('staging-rollout', name)
            )
            self.assertIn('"$STAGING_ATTEMPT" == "$GITHUB_RUN_ATTEMPT"', workflow(name))

    def test_manual_actions_only_offer_prod_and_staging_uses_all_commit_tags(self):
        for name in ('deploy', 'publish'):
            dispatch = (
                workflow(name).split('  workflow_dispatch:\n', 1)[1].split('  workflow_call:', 1)[0]
            )
            self.assertIn('options: [prod]', dispatch)
            self.assertNotIn('options: [staging', dispatch)
        deploy = job('deploy', 'deploy')
        for flag in ('--tag', '--serve-tag', '--hostd-tag'):
            self.assertIn(flag + ' "$SOURCE_TAG"', deploy)
        self.assertIn('--keep-hosting', deploy)
        self.assertIn('for image in serve hostd;', deploy)
        publish = job('publish', 'publish')
        self.assertIn('"$RUN_ID" == "$GITHUB_RUN_ID"', publish)
        self.assertIn('run-id: ${{ inputs.run_id }}', publish)
        # Production still promotes staging's recorded digests and publishes release builds.
        self.assertIn('--deployment prod app deploy --images-from - --yes', deploy)
        self.assertIn('environment: staging', job('deploy', 'staging-release'))
        self.assertIn('"$workflow" == .github/workflows/release.yml', publish)


class DeploymentCommand(unittest.TestCase):
    """Execute the workflow's actual shell command with registry/deploy interfaces faked."""

    def run_deploy(self, **environment):
        body = job('deploy', 'deploy').split('      - name: app deploy\n', 1)[1]
        body = body.split('      - name:', 1)[0].split('        run: |\n', 1)[1]
        script = textwrap.dedent(body)
        with tempfile.TemporaryDirectory() as directory:
            command = Path(directory) / 'deploy' / 'bin' / 'on-deploy'
            command.parent.mkdir(parents=True)
            command.write_text(
                '#!/usr/bin/env bash\nprintf "deploy %s\\n" "$*"\n'
                'if [[ "$*" == *--images-from* ]]; then cat; fi\n',
                encoding='utf-8',
            )
            command.chmod(0o755)
            env = os.environ | {
                'IN_DEPLOYMENT': 'staging',
                'SOURCE_TAG': 'sha-aaaaaaa',
                'IN_PROMOTED': '',
                'IN_NO_HOSTING': 'false',
                'GITHUB_REPOSITORY_OWNER': 'example',
                'GITHUB_RUN_ATTEMPT': '1',
                'STAGING_ATTEMPT': '1',
                **environment,
            }
            # Intercept docker, including the missing-image path. No registry or
            # AWS calls can leave this test: on-deploy is a temporary executable.
            registry = 'docker() { [[ "$*" != *"${MISSING_IMAGE:-never-match}"* ]]; }\n'
            return subprocess.run(
                [shutil.which('bash'), '--noprofile', '--norc', '-c', registry + script],
                cwd=directory,
                env=env,
                capture_output=True,
                text=True,
            )

    def test_staging_pins_every_image_and_preserves_hosting(self):
        result = self.run_deploy()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            'deploy --deployment staging app deploy --tag sha-aaaaaaa '
            '--serve-tag sha-aaaaaaa --hostd-tag sha-aaaaaaa --keep-hosting',
        )

    def test_missing_hosting_image_invalid_tag_and_old_attempt_never_deploy(self):
        for environment in (
            {'MISSING_IMAGE': 'opennova-hostd'},
            {'MISSING_IMAGE': 'opennova-serve'},
            {'SOURCE_TAG': ''},
            {'SOURCE_TAG': 'latest'},
            {'GITHUB_RUN_ATTEMPT': '2'},
            {'IN_DEPLOYMENT': 'unknown'},
        ):
            with self.subTest(environment=environment):
                result = self.run_deploy(**environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('deploy --deployment', result.stdout)

    def test_prod_promotes_the_recorded_manifest_and_honors_hosting_choice(self):
        manifest = '{"deployment":"staging","release":"previous","images":{}}'
        result = self.run_deploy(IN_DEPLOYMENT='prod', IN_PROMOTED=manifest, IN_NO_HOSTING='true')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            'deploy --deployment prod app deploy --images-from - --yes --no-hosting\n' + manifest,
        )
        result = self.run_deploy(IN_DEPLOYMENT='prod')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('deploy --deployment', result.stdout)


if __name__ == '__main__':
    unittest.main()
