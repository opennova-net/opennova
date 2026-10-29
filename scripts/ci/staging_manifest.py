#!/usr/bin/env python3
"""Pin one staging CI attempt's ten image digests and four package artifacts.

The readiness job creates release.json only after the component jobs succeed.
Rollout validates that immutable artifact before credentials, then deploy/publish
read the same artifact ID. Unrelated checks (or cancellation after dispatch) do
not revoke a release whose components already passed.
"""

import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
import zipfile
from datetime import datetime
from pathlib import Path

from staging_source import gh_api, validate

IMAGE_REPOSITORIES = {
    **{key: f'opennova-novaworld-{key}' for key in ('web', 'data', 'lobby', 'gate', 'worker', 'fleet', 'backup')},
    'portal': 'novaworld-web',
    'serve': 'opennova-serve',
    'hostd': 'opennova-hostd',
}
PACKAGE_PATTERNS = {
    'runtime': r'opennova-runtime-windows-v[0-9.]+\.zip',
    'editor': r'opennova-editor-windows-v[0-9.]+\.zip',
    'tools': r'opennova-tools-windows\.zip',
    'blender': r'opennova-blender-addon-windows\.zip',
}
DIGEST = r'sha256:[0-9a-f]{64}'
READINESS_JOB = 'staging-components-ready'


def pages(path: str, key: str) -> list[dict]:
    result = []
    separator = '&' if '?' in path else '?'
    page = 1
    while True:
        items = gh_api(f'{path}{separator}per_page=100&page={page}')[key]
        result.extend(items)
        if len(items) < 100:
            return result
        page += 1


def source(
    repository: str, run_id: str, attempt: str, sha: str, *, require_current_attempt: bool = False
) -> dict:
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('repository must be owner/name')
    if not re.fullmatch(r'[1-9][0-9]*', run_id):
        raise ValueError('run-id must be numeric')
    run = gh_api(f'repos/{repository}/actions/runs/{run_id}')
    validate(run, repository, run_id, sha, 'refs/heads/staging')
    if (not re.fullmatch(r'[1-9][0-9]*', str(attempt))
            or not isinstance(run.get('run_attempt'), int)
            or int(attempt) > run['run_attempt']):
        raise ValueError('the requested source attempt does not exist')
    if require_current_attempt:
        validate(run, repository, run_id, sha, 'refs/heads/staging', attempt)
    # The attempt endpoint supplies its own start time, including a full rerun.
    # An admitted rollout can finish using this attempt even if CI is rerun;
    # only creation/cancellation/dispatch require the latest attempt.
    attempt_run = gh_api(f'repos/{repository}/actions/runs/{run_id}/attempts/{attempt}')
    validate(attempt_run, repository, run_id, sha, 'refs/heads/staging', attempt)
    return attempt_run


def timestamp(value: str) -> datetime:
    if not value:
        raise ValueError('missing artifact/attempt timestamp')
    return datetime.fromisoformat(value.replace('Z', '+00:00'))


def artifact_ok(artifact: dict, run: dict) -> None:
    provenance = artifact.get('workflow_run') or {}
    if artifact.get('expired') or not isinstance(artifact.get('id'), int):
        raise ValueError('artifact expired or has no immutable ID')
    if provenance.get('id') != run['id'] or provenance.get('head_sha') != run['head_sha']:
        raise ValueError('artifact belongs to another source run or commit')
    if timestamp(artifact.get('created_at')) < timestamp(run.get('run_started_at')):
        raise ValueError('artifact belongs to an earlier attempt; start a new manual staging CI run for a complete rebuild')
    if not re.fullmatch(DIGEST, artifact.get('digest') or ''):
        raise ValueError('artifact has no SHA-256 checksum')


def artifact_bytes(repository: str, artifact: dict) -> bytes:
    # GitHub's /zip redirect returns the original upload, including archive:false
    # files. Its digest hashes those bytes before any archive extraction.
    response = subprocess.run(
        ['gh', 'api', f'repos/{repository}/actions/artifacts/{artifact["id"]}/zip'],
        check=True, capture_output=True,
    )
    data = response.stdout
    if 'sha256:' + hashlib.sha256(data).hexdigest() != artifact['digest']:
        raise ValueError(f'artifact {artifact["id"]} checksum mismatch')
    return data


def artifact_json(repository: str, artifact: dict, filename: str) -> dict:
    with zipfile.ZipFile(io.BytesIO(artifact_bytes(repository, artifact))) as archive:
        files = [item for item in archive.infolist() if not item.is_dir()]
        if len(files) != 1 or files[0].filename != filename or files[0].file_size > 65536:
            raise ValueError(f'artifact must contain only {filename} (at most 64 KiB)')
        return json.loads(archive.read(filename))


def identity(repository: str, run: dict) -> dict:
    return {'repository': repository, 'source': {
        'run_id': run['id'], 'run_attempt': run['run_attempt'], 'sha': run['head_sha'],
    }}


def create(repository: str, run: dict) -> dict:
    artifacts = pages(f'repos/{repository}/actions/runs/{run["id"]}/artifacts', 'artifacts')
    manifest = {'schema': 1, **identity(repository, run), 'images': {}, 'packages': {}}
    owner = repository.split('/')[0].lower()
    for key, image in IMAGE_REPOSITORIES.items():
        name = f'image-digest-{key}-{run["run_attempt"]}'
        matches = [item for item in artifacts if item['name'] == name]
        if len(matches) != 1:
            raise ValueError(f'expected exactly one {name} artifact')
        artifact_ok(matches[0], run)
        record = artifact_json(repository, matches[0], f'{key}.json')
        expected = {'image': key, 'repository': f'ghcr.io/{owner}/{image}',
                    'source_sha': run['head_sha'], 'run_id': run['id'], 'run_attempt': run['run_attempt']}
        if any(record.get(field) != value for field, value in expected.items()):
            raise ValueError(f'{key} digest record has mismatched source or repository')
        if not re.fullmatch(DIGEST, record.get('digest') or ''):
            raise ValueError(f'{key} is not pinned by digest')
        manifest['images'][key] = record['repository'] + '@' + record['digest']
    for key, pattern in PACKAGE_PATTERNS.items():
        matches = [item for item in artifacts if re.fullmatch(pattern, item['name'])]
        if len(matches) != 1:
            raise ValueError(f'expected exactly one {key} package artifact')
        artifact = matches[0]
        artifact_ok(artifact, run)
        manifest['packages'][key] = {
            'id': artifact['id'], 'name': artifact['name'], 'sha256': artifact['digest'][7:],
        }
    return manifest


def validate_manifest(manifest: dict, repository: str, run: dict) -> None:
    if manifest.get('schema') != 1 or any(manifest.get(k) != v for k, v in identity(repository, run).items()):
        raise ValueError('manifest belongs to another source run, attempt, repository, or commit')
    images, packages = manifest.get('images', {}), manifest.get('packages', {})
    if set(images) != set(IMAGE_REPOSITORIES) or set(packages) != set(PACKAGE_PATTERNS):
        raise ValueError('manifest must pin all ten images and four packages')
    owner = repository.split('/')[0].lower()
    for key, image in IMAGE_REPOSITORIES.items():
        if not re.fullmatch(re.escape(f'ghcr.io/{owner}/{image}@') + DIGEST, images[key]):
            raise ValueError(f'invalid {key} image digest/repository')
    for key, package in packages.items():
        if (not isinstance(package.get('id'), int) or package['id'] <= 0
                or not re.fullmatch(PACKAGE_PATTERNS[key], package.get('name') or '')
                or not re.fullmatch(r'[0-9a-f]{64}', package.get('sha256') or '')):
            raise ValueError(f'invalid {key} package identity/checksum')
    if len({item['id'] for item in packages.values()}) != 4:
        raise ValueError('package artifact IDs must be distinct')


def load(repository: str, run: dict, manifest_id: str) -> dict:
    if not re.fullmatch(r'[1-9][0-9]*', manifest_id):
        raise ValueError('manifest-id must be numeric')
    jobs = pages(
        f'repos/{repository}/actions/runs/{run["id"]}/attempts/{run["run_attempt"]}/jobs', 'jobs'
    )
    readiness = [job for job in jobs if job.get('name') == READINESS_JOB]
    if (len(readiness) != 1 or readiness[0].get('status') != 'completed'
            or readiness[0].get('conclusion') != 'success'):
        raise ValueError('source attempt has not successfully completed staging component readiness')
    artifact = gh_api(f'repos/{repository}/actions/artifacts/{manifest_id}')
    artifact_ok(artifact, run)
    if artifact['id'] != int(manifest_id) or artifact['name'] != f'staging-release-{run["run_attempt"]}':
        raise ValueError('not the immutable staging release artifact for this attempt')
    manifest = artifact_json(repository, artifact, 'release.json')
    validate_manifest(manifest, repository, run)
    return manifest


def download_packages(repository: str, run: dict, manifest: dict, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if any(destination.iterdir()):
        raise ValueError('package destination must be empty')
    for package in manifest['packages'].values():
        artifact = gh_api(f'repos/{repository}/actions/artifacts/{package["id"]}')
        artifact_ok(artifact, run)
        if (artifact['id'] != package['id'] or artifact['name'] != package['name']
                or artifact['digest'] != 'sha256:' + package['sha256']):
            raise ValueError('package metadata no longer matches the admitted release')
        (destination / package['name']).write_bytes(artifact_bytes(repository, artifact))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['create', 'read', 'packages'])
    parser.add_argument('--repository', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--attempt', required=True)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--manifest-id')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--latest', action='store_true')
    args = parser.parse_args(argv)
    try:
        run = source(args.repository, args.run_id, args.attempt, args.sha,
                     require_current_attempt=args.command == 'create')
        manifest = (create(args.repository, run) if args.command == 'create'
                    else load(args.repository, run, args.manifest_id or ''))
        if args.command == 'packages':
            if args.out is None:
                raise ValueError('packages requires --out directory')
            download_packages(args.repository, run, manifest, args.out)
        elif args.out:
            args.out.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        current = True
        if args.latest:
            head = gh_api(f'repos/{args.repository}/git/ref/heads/staging').get('object', {}).get('sha')
            if not isinstance(head, str) or not re.fullmatch(r'[0-9a-f]{40}', head):
                raise ValueError('could not read the current staging head')
            current = head == args.sha
        print(json.dumps({'current': current, 'manifest': manifest}))
        return 0
    except (ValueError, TypeError, KeyError, OSError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f'staging_manifest: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
