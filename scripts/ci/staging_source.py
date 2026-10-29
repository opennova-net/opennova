#!/usr/bin/env python3
"""Validate the CI run that may release to staging; optionally skip a superseded head.

Called inside the serialized rollout, and again at its deploy and publish entry points.
The run is still in progress: component dependencies gate rollout, not its conclusion.
All GitHub context is passed explicitly; this command reads APIs and prints JSON only.
"""

import argparse
import json
import re
import subprocess
import sys


def gh_api(path: str) -> dict:
    result = subprocess.run(
        ['gh', 'api', path], check=True, capture_output=True, text=True, encoding='utf-8'
    )
    return json.loads(result.stdout)


def validate(
    run: dict, repository: str, run_id: str, sha: str, ref: str, attempt: str | None = None
) -> None:
    if ref != 'refs/heads/staging':
        raise ValueError('staging releases require refs/heads/staging, never a PR or tag ref')
    if run.get('id') != int(run_id):
        raise ValueError('the source run ID does not match the current CI run')
    if any(
        (run.get(key) or {}).get('full_name') != repository
        for key in ('repository', 'head_repository')
    ):
        raise ValueError('the build must come from this repository, not a fork')
    if run.get('path') != '.github/workflows/ci.yml':
        raise ValueError('staging releases require a CI workflow run')
    if run.get('event') not in ('push', 'workflow_dispatch') or run.get('head_branch') != 'staging':
        raise ValueError('staging releases require a push or manual CI run on the staging branch')
    if not re.fullmatch(r'[0-9a-f]{40}', sha) or run.get('head_sha') != sha:
        raise ValueError('the source run must have built the current checkout commit')
    if attempt is not None and (
        not re.fullmatch(r'[1-9][0-9]*', str(attempt))
        or run.get('run_attempt') != int(attempt)
    ):
        raise ValueError('the source attempt changed; start a new manual staging CI run for a complete rebuild')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--ref', required=True)
    parser.add_argument('--attempt', help='the immutable source CI attempt, not this rollout attempt')
    parser.add_argument('--latest', action='store_true', help='skip a superseded staging head')
    args = parser.parse_args(argv)
    try:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', args.repository):
            raise ValueError('repository must be owner/name')
        if not re.fullmatch(r'[0-9]+', args.run_id):
            raise ValueError('run-id must be numeric')
        run = gh_api(f'repos/{args.repository}/actions/runs/{args.run_id}')
        validate(run, args.repository, args.run_id, args.sha, args.ref, args.attempt)
        current = True
        if args.latest:
            branch = gh_api(f'repos/{args.repository}/git/ref/heads/staging')
            head = branch.get('object', {}).get('sha') or ''
            if not re.fullmatch(r'[0-9a-f]{40}', head):
                raise ValueError('could not read the current staging head')
            current = head == args.sha
        print(json.dumps({'sha': args.sha, 'tag': f'sha-{args.sha[:7]}', 'current': current}))
        return 0
    except (ValueError, subprocess.CalledProcessError, OSError) as error:
        print(f'staging_source: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
