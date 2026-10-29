#!/usr/bin/env python3
"""Keep required check names stable while verifying the selected component graph."""

import argparse
import json
import sys


GROUPS = {
    'native': {
        'gates': (),
        'test-core': ('native',),
        'test-linux': ('native',),
        'service-build': ('service', 'stack'),
        'service-images': ('service',),
        'serve-images': ('service',),
        'local-stack': ('stack',),
        'web': ('web',),
        'infra': ('infra',),
        'package-blender-windows': ('blender',),
        'package-apps-windows': ('tools',),
    },
    'godot': {
        'build-gdextension-windows': ('godot',),
        'build-gdextension-linux': ('godot',),
        'godot-tests-core': ('godot',),
        'godot-tests-linux': ('godot',),
        'godot-tests-windowed': ('godot',),
        'package-godot-windows': ('godot',),
    },
}


def check(group: str, needs: dict) -> list[str]:
    errors = []
    changes = needs.get('changes', {})
    if changes.get('result') != 'success':
        return ['changes did not succeed']
    selected = changes.get('outputs', {})
    for job, components in GROUPS[group].items():
        if any(selected.get(component) not in ('true', 'false') for component in components):
            errors.append(f'{job}: invalid or absent component selection')
            continue
        expected = 'success' if not components or any(selected[c] == 'true' for c in components) else 'skipped'
        actual = needs.get(job, {}).get('result', 'missing')
        if actual != expected:
            errors.append(f'{job}: {actual}; expected {expected}')
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', required=True, choices=GROUPS)
    parser.add_argument('--needs', required=True)
    args = parser.parse_args()
    errors = check(args.group, json.loads(args.needs))
    for error in errors:
        print(f'::error::{error}')
    return int(bool(errors))


if __name__ == '__main__':
    sys.exit(main())
