#!/usr/bin/env python3
"""Carry one validated Linux service build into its images and stack smoke test."""

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile


ROOT = Path(__file__).resolve().parents[2]
BINARIES = [
    *[f'apps/novaworld_{name}/opennova-novaworld-{name}' for name in
      ('gate', 'lobby', 'web', 'fleet', 'worker', 'data')],
    'apps/serve/opennova-serve',
    'apps/hostd/opennova-hostd',
    'apps/project/opennova-project',
    'apps/update_tool/opennova-update-tool',
    'tests/opennova-novaworld-stack-smoke',
]
SOURCE_DIRS = ('backend/migrations', 'backend/seed',
               'apps/novaworld_web/templates', 'apps/novaworld_web/static')
SOURCE_FILES = ('apps/novaworld_common/hosted/cloud/machine/machine_user_data.sh',)


def identity(sha, run_id, attempt):
    if not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise ValueError('source SHA must be a full commit ID')
    if not re.fullmatch(r'[1-9][0-9]*', run_id) or not re.fullmatch(r'[1-9][0-9]*', attempt):
        raise ValueError('source run and attempt must be positive integers')
    return {'schema': 1, 'sha': sha, 'run_id': run_id, 'run_attempt': attempt,
            'platform': 'ubuntu-24.04-x86_64'}


def one_stamp(version, staging, commit, build_time):
    values = {'version': version, 'staging': staging, 'commit': commit, 'time': build_time}
    patterns = {'version': r'[0-9]+(\.[0-9]+){0,3}', 'staging': r'ON|OFF',
                'commit': r'[0-9a-f]{7}', 'time': r'[0-9]+'}
    if any(not isinstance(values[key], str) or not re.fullmatch(pattern, values[key])
           for key, pattern in patterns.items()):
        raise ValueError('invalid ONE stamp in service bundle')
    return values


def pack(build, output, source, *, root=ROOT, service_tests=False, one=None):
    one = one_stamp(one.get('version'), one.get('staging'), one.get('commit'), one.get('time')) if one is not None else None
    if one is None:
        raise ValueError('service bundle requires its compiled ONE stamp')
    files = {f'src/build/{name}': build / name for name in BINARIES}
    for directory in SOURCE_DIRS:
        files.update({f'src/{path.relative_to(root).as_posix()}': path
                      for path in sorted((root / directory).rglob('*')) if path.is_file()})
    files.update({f'src/{name}': root / name for name in SOURCE_FILES})
    manifest = {**source, 'one': one, 'service_tests': service_tests, 'files': {}}
    with tarfile.open(output, 'w:gz') as archive:
        for name, path in files.items():
            if path.is_symlink() or not path.is_file():
                raise ValueError(f'missing or nonregular build input: {path}')
            data = path.read_bytes()
            mode = 0o755 if name.startswith('src/build/') else 0o644
            manifest['files'][name] = {'sha256': hashlib.sha256(data).hexdigest(), 'mode': mode}
            member = tarfile.TarInfo(name)
            member.size, member.mode = len(data), mode
            archive.addfile(member, io.BytesIO(data))
        data = json.dumps(manifest, sort_keys=True).encode()
        member = tarfile.TarInfo('manifest.json')
        member.size = len(data)
        archive.addfile(member, io.BytesIO(data))


def unpack(archive_path, destination, source, *, require_service_tests=False):
    # Validate the complete inventory before writing any output. Artifacts from
    # another commit/run/attempt, duplicate members and links never become images.
    with tarfile.open(archive_path, 'r:gz') as archive:
        members = archive.getmembers()
        if len({member.name for member in members}) != len(members):
            raise ValueError('duplicate bundle member')
        if any(not member.isfile() for member in members):
            raise ValueError('bundle may contain regular files only')
        manifest = json.load(archive.extractfile('manifest.json'))
        if any(manifest.get(key) != value for key, value in source.items()):
            raise ValueError('bundle source or platform does not match this run')
        stamp = manifest.get('one', {})
        one_stamp(stamp.get('version'), stamp.get('staging'), stamp.get('commit'), stamp.get('time'))
        if require_service_tests and manifest.get('service_tests') is not True:
            raise ValueError('image publication requires the service test gate')
        files = manifest.get('files', {})
        if set(files) != {member.name for member in members} - {'manifest.json'}:
            raise ValueError('bundle inventory does not match its manifest')
        if not {f'src/build/{name}' for name in BINARIES}.issubset(files):
            raise ValueError('bundle is missing a service binary')
        for name, record in files.items():
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or not name.startswith('src/') or '\\' in name:
                raise ValueError(f'unsafe bundle path: {name}')
            data = archive.extractfile(name).read()
            if hashlib.sha256(data).hexdigest() != record['sha256']:
                raise ValueError(f'bundle checksum mismatch: {name}')
            if record['mode'] not in (0o644, 0o755):
                raise ValueError(f'unsupported bundle mode: {name}')
        if destination.exists():
            raise ValueError(f'bundle destination already exists: {destination}')
        destination.mkdir(parents=True)
        for name, record in files.items():
            output = destination / name
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open('wb') as target:
                shutil.copyfileobj(archive.extractfile(name), target)
            output.chmod(record['mode'])
        (destination / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('pack', 'unpack'))
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--to', type=Path, default=Path('build/service-bundle'))
    parser.add_argument('--sha', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--run-attempt', required=True)
    parser.add_argument('--service-tests', action='store_true')
    parser.add_argument('--require-service-tests', action='store_true')
    parser.add_argument('--one-version')
    parser.add_argument('--one-staging')
    parser.add_argument('--one-commit')
    parser.add_argument('--one-time')
    args = parser.parse_args()
    source = identity(args.sha, args.run_id, args.run_attempt)
    if args.command == 'pack':
        pack(args.build, args.archive, source, service_tests=args.service_tests,
             one={'version': args.one_version, 'staging': args.one_staging,
                  'commit': args.one_commit, 'time': args.one_time})
    else:
        unpack(args.archive, args.to, source, require_service_tests=args.require_service_tests)


if __name__ == '__main__':
    main()
