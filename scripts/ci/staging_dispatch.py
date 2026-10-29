#!/usr/bin/env python3
"""Bootstrap detached staging rollouts without cancelling legacy reusable rolls.

An early CI probe freezes each attempt's mode. Only a verified detached attempt
uploads staging-detached-N. Cancellation requires that marker on BOTH the new
attempt and each older attempt. Unmarked legacy runs always finish, including
while the dispatcher is first being installed on the default branch.
"""

import argparse
import base64
import json
import re
import subprocess
import sys
from urllib.parse import quote

from staging_manifest import artifact_ok, load, pages, source
from staging_source import gh_api, validate

PROTOCOL = '# staging-dispatch-protocol: 1'


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
    if PROTOCOL not in workflow or '\n  workflow_dispatch:\n' not in workflow:
        return False
    dispatch = workflow.split('\n  workflow_dispatch:\n', 1)[1].split('\n  workflow_call:', 1)[0]
    if any(not re.search(r'^      ' + name + r':$', dispatch, re.M)
           for name in ('run_id', 'source_attempt', 'source_sha', 'manifest_id')):
        return False
    registered = optional_api(f'repos/{repository}/actions/workflows/staging-rollout.yml')
    return registered is not None and registered.get('state') == 'active'


def current(repository: str, sha: str) -> bool:
    head = gh_api(f'repos/{repository}/git/ref/heads/staging').get('object', {}).get('sha')
    if not isinstance(head, str) or not re.fullmatch(r'[0-9a-f]{40}', head):
        raise ValueError('could not read the current staging head')
    return head == sha


def marked(repository: str, run: dict) -> bool:
    artifacts = pages(f'repos/{repository}/actions/runs/{run["id"]}/artifacts', 'artifacts')
    markers = [item for item in artifacts if item['name'] == f'staging-detached-{run["run_attempt"]}']
    if len(markers) != 1:
        return False
    try:
        artifact_ok(markers[0], run)
        return True
    except (ValueError, TypeError):
        return False


def cancel_older(repository: str, run: dict) -> list[int]:
    if not available(repository) or not current(repository, run['head_sha']) or not marked(repository, run):
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
        if not marked(repository, candidate):
            continue
        # A slower old probe must never cancel work once another commit wins.
        if not current(repository, run['head_sha']):
            break
        request(f'repos/{repository}/actions/runs/{candidate["id"]}/cancel')
        cancelled.append(candidate['id'])
    return cancelled


def dispatch(repository: str, run: dict, manifest_id: str) -> bool:
    if not available(repository):
        raise ValueError('detached dispatcher disappeared; do not fall back after enabling cancellation')
    if not marked(repository, run):
        raise ValueError('this attempt did not opt in to detached rollout before building')
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
    parser.add_argument('command', choices=['probe', 'cancel-older', 'dispatch'])
    parser.add_argument('--repository', required=True)
    parser.add_argument('--run-id')
    parser.add_argument('--attempt')
    parser.add_argument('--sha')
    parser.add_argument('--manifest-id')
    args = parser.parse_args(argv)
    try:
        if args.command == 'probe':
            result = {'detached': available(args.repository)}
        else:
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
