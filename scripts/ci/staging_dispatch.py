#!/usr/bin/env python3
"""Dispatch a staging CI run's rollout as a run of its own, and cancel superseded builds.

A staging CI run freezes its validated components into a release manifest
(staging_manifest.py) and dispatches the Staging rollout workflow with it: the deploy
runs in its own run, under the staging-rollout lock, so cancelling the source build never
reaches a deploy. GitHub dispatches a workflow only as the default branch declares it, so
the dispatch first checks that the default branch carries staging-rollout.yml with the
protocol line and the four inputs. Each staging run's first job cancels the older staging
builds still running.
"""

import argparse
import base64
import json
import re
import subprocess
import sys
from urllib.parse import quote

from staging_manifest import load, pages, source
from staging_source import gh_api, validate

PROTOCOL = '# staging-dispatch-protocol: 1'
INPUTS = ('run_id', 'source_attempt', 'source_sha', 'manifest_id')


def request(path: str, payload: dict | None = None) -> None:
    subprocess.run(
        ['gh', 'api', '--method', 'POST', path, '--input', '-'],
        input=json.dumps(payload or {}), text=True, capture_output=True, check=True,
    )


def optional_api(path: str) -> dict | None:
    try:
        return gh_api(path)
    except subprocess.CalledProcessError as error:
        if 'HTTP 404' in (error.stderr or ''):
            return None
        raise


def dispatch_inputs(workflow: str) -> str:
    """The workflow_dispatch block's lines, up to the next key at its depth or less."""
    lines = workflow.split('\n')
    if '  workflow_dispatch:' not in lines:
        return ''
    block = []
    for line in lines[lines.index('  workflow_dispatch:') + 1:]:
        if line.strip() and len(line) - len(line.lstrip(' ')) <= 2:
            break
        block.append(line)
    return '\n'.join(block)


def available(repository: str) -> bool:
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('repository must be owner/name')
    repo = gh_api(f'repos/{repository}')
    default = repo['default_branch']
    content = optional_api(
        f'repos/{repository}/contents/.github/workflows/staging-rollout.yml?ref={quote(default, safe="")}'
    )
    if content is None:
        return False
    workflow = base64.b64decode(content['content']).decode('utf-8')
    inputs = dispatch_inputs(workflow)
    if PROTOCOL not in workflow or not inputs:
        return False
    if any(not re.search(r'^      ' + name + r':$', inputs, re.M) for name in INPUTS):
        return False
    registered = optional_api(f'repos/{repository}/actions/workflows/staging-rollout.yml')
    return registered is not None and registered.get('state') == 'active'


def current(repository: str, sha: str) -> bool:
    head = gh_api(f'repos/{repository}/git/ref/heads/staging').get('object', {}).get('sha')
    if not isinstance(head, str) or not re.fullmatch(r'[0-9a-f]{40}', head):
        raise ValueError('could not read the current staging head')
    return head == sha


def cancel_older(repository: str, run: dict) -> list[int]:
    """The older staging CI runs still running, cancelled: builds a newer push superseded.
    A deploy runs in its own dispatched rollout run, which this never reaches."""
    if not current(repository, run['head_sha']):
        return []
    cancelled = []
    # Query active runs only; scanning the entire CI history grows unbounded.
    candidates = {}
    for status in ('in_progress', 'queued'):
        for candidate in pages(
            f'repos/{repository}/actions/workflows/ci.yml/runs?branch=staging&status={status}', 'workflow_runs'
        ):
            candidates[candidate['id']] = candidate
    for candidate in candidates.values():
        if (candidate.get('status') not in ('queued', 'in_progress', 'waiting', 'pending', 'requested')
                or candidate.get('run_number', 0) >= run['run_number']):
            continue
        candidate = gh_api(f'repos/{repository}/actions/runs/{candidate["id"]}')
        try:
            validate(candidate, repository, str(candidate['id']), candidate['head_sha'],
                     'refs/heads/staging', str(candidate['run_attempt']))
        except (ValueError, KeyError):
            continue
        if candidate.get('status') == 'completed' or candidate['run_number'] >= run['run_number']:
            continue
        # A slower older run must never cancel work once another commit wins.
        if not current(repository, run['head_sha']):
            break
        request(f'repos/{repository}/actions/runs/{candidate["id"]}/cancel')
        cancelled.append(candidate['id'])
    return cancelled


def dispatch(repository: str, run: dict, manifest_id: str) -> bool:
    if not available(repository):
        raise ValueError('the default branch carries no staging dispatcher (staging-rollout.yml '
                         'with the dispatch protocol and its four inputs): merge it there first')
    load(repository, run, manifest_id)
    if not current(repository, run['head_sha']):
        return False
    request(f'repos/{repository}/actions/workflows/staging-rollout.yml/dispatches', {
        'ref': 'staging',
        'inputs': {'run_id': str(run['id']), 'source_attempt': str(run['run_attempt']),
                   'source_sha': run['head_sha'], 'manifest_id': manifest_id},
    })
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['cancel-older', 'dispatch'])
    parser.add_argument('--repository', required=True)
    parser.add_argument('--run-id')
    parser.add_argument('--attempt')
    parser.add_argument('--sha')
    parser.add_argument('--manifest-id')
    args = parser.parse_args(argv)
    try:
        if not all((args.run_id, args.attempt, args.sha)):
            raise ValueError('run-id, attempt and sha are required')
        run = source(args.repository, args.run_id, args.attempt, args.sha,
                     require_current_attempt=True)
        if args.command == 'cancel-older':
            result = {'cancelled': cancel_older(args.repository, run)}
        else:
            result = {'dispatched': dispatch(args.repository, run, args.manifest_id or '')}
        print(json.dumps(result))
        return 0
    except (ValueError, TypeError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f'staging_dispatch: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
