#!/usr/bin/env python3
"""Select complete CI components for a PR; uncertain inputs run the full suite."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


COMPONENTS = ('native', 'godot', 'service', 'web', 'blender', 'infra', 'stack', 'tools')
SERVICE = {'service', 'web', 'stack'}


def classify(paths: list[str] | None) -> dict[str, bool]:
    """None/empty means unknown, not a change proven to affect nothing."""
    selected = set()
    if not paths:
        selected.update(COMPONENTS)
    for path in paths or []:
        if not isinstance(path, str) or not path or path.startswith('/') or '..' in path.split('/'):
            return dict.fromkeys(COMPONENTS, True)
        if path == 'docs' or path.endswith('.md'):
            continue
        if path.startswith(('godot/', 'third_party/godot-cpp', 'third_party/gut')):
            selected.add('godot')
        elif path.startswith(('tools/blender/', 'tests/blender/')):
            selected.add('blender')
        elif path.startswith(('tests/fixtures/', 'fixtures/')):
            selected.update(COMPONENTS)
        elif path in ('tests/novaworld/stack_smoke.cpp', 'tests/novaworld/lobby_client.h',
                      'tests/serve/serve_test_support.h', 'tests/novaworld/service_tests.cmake'):
            # The stack executable has no CTest row; native compilation alone
            # cannot validate changes to it or its runtime helpers.
            selected.update(('native', 'web', 'stack'))
        elif path.startswith('tests/'):
            selected.add('native')
        elif re.match(r'apps/novaworld_[^/]+/', path) or path.startswith('backend/'):
            selected.update(SERVICE)
            if path.startswith('apps/novaworld_common/hosted/cloud/machine/'):
                selected.add('infra')
        elif path.startswith('web/'):
            selected.update(('web', 'stack'))
            if path == 'web/docker-entrypoint.sh':
                selected.add('infra')
        elif path.startswith('infra/'):
            selected.add('infra')
        elif path.startswith('deploy/') or path == 'scripts/dev_stack.sh':
            selected.update(SERVICE | {'infra'})
        elif path.startswith('scripts/lint/'):
            # Gates always run, including the scripts' own unit tests.
            continue
        else:
            # Engine/app libraries, build configuration, workflow code and any
            # new component need the entire graph until explicitly classified.
            selected.update(COMPONENTS)
    return {component: component in selected for component in COMPONENTS}


def changed_paths(event: dict, repository: str) -> list[str]:
    pr = event['pull_request']
    number = int(event['number'])
    expected = int(pr['changed_files'])
    if number <= 0 or expected <= 0 or not re.fullmatch(r'[\w.-]+/[\w.-]+', repository):
        raise ValueError('invalid pull request metadata')
    # GitHub caps this endpoint at 3,000 files. Compare with the PR's own count
    # instead of treating a successfully paginated but truncated list as complete.
    result = subprocess.run(
        ['gh', 'api', f'repos/{repository}/pulls/{number}/files?per_page=100',
         '--paginate', '--slurp'],
        check=True, capture_output=True, text=True, encoding='utf-8',
    )
    pages = json.loads(result.stdout)
    if not isinstance(pages, list) or not all(isinstance(page, list) for page in pages):
        raise ValueError('expected paginated file lists')
    files = [file for page in pages for file in page]
    if len(files) != expected:
        raise ValueError(f'file list has {len(files)} entries; expected {expected}')
    paths = []
    for file in files:
        path = file['filename']
        if not isinstance(path, str) or not path:
            raise ValueError('missing changed filename')
        paths.append(path)
        previous = file.get('previous_filename')
        if previous is not None:
            if not isinstance(previous, str) or not previous:
                raise ValueError('invalid previous filename')
            paths.append(previous)
    return paths


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--event', required=True)
    parser.add_argument('--event-path', type=Path, required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--github-output', type=Path)
    parser.add_argument('--summary', type=Path)
    args = parser.parse_args(argv)
    reason = 'Branch/manual runs validate every component.'
    paths = None
    if args.event == 'pull_request':
        try:
            paths = changed_paths(json.loads(args.event_path.read_text(encoding='utf-8')), args.repository)
            reason = 'Selected from the complete PR diff, including renamed paths.'
        except (KeyError, TypeError, ValueError, OSError, subprocess.CalledProcessError) as error:
            reason = f'Could not prove the changed paths; running everything ({type(error).__name__}).'
            print(f'::warning::{reason}', file=sys.stderr)
    selection = classify(paths)
    outputs = {**selection, 'code': any(selection.values())}
    if args.github_output:
        with args.github_output.open('a', encoding='utf-8') as output:
            for key, value in outputs.items():
                output.write(f'{key}={str(value).lower()}\n')
    if args.summary:
        with args.summary.open('a', encoding='utf-8') as summary:
            summary.write('### CI components\n\n' + reason + '\n\n')
            summary.write(', '.join(name for name, enabled in selection.items() if enabled) or 'Repository gates only')
            summary.write('\n')
    print(json.dumps(outputs, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
