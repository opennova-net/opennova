"""Delivery workflows are called once by CI and consume its validated builds."""

from pathlib import Path
import re
import unittest

from service_bundle import BINARIES


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github' / 'workflows'


def dockerfile_build_targets(dockerfile):
    text = dockerfile.read_text(encoding='utf-8')
    start = text.index('cmake --build build --target')
    command = []
    for line in text[start:].splitlines():
        command.append(line.rstrip('\\').strip())
        if not line.rstrip().endswith('\\'):
            break
    return [token for token in ' '.join(command).split() if token.startswith('opennova_')]


class SharedDeliveryBuild(unittest.TestCase):
    def test_ci_owns_automatic_delivery_triggers(self):
        for name in ('novaworld-images', 'serve-image', 'local-stack'):
            with self.subTest(workflow=name):
                text = (WORKFLOWS / f'{name}.yml').read_text(encoding='utf-8')
                events = text.split('\non:\n', 1)[1].split('\nconcurrency:', 1)[0]
                self.assertIn('  workflow_call:\n', events)
                self.assertIn('  workflow_dispatch:\n', events)
                self.assertNotRegex(events, re.compile(r'^  (push|pull_request):', re.M))
                self.assertNotIn('submodules: recursive', text)

    def test_all_native_image_targets_are_in_the_shared_bundle(self):
        for dockerfile in ('apps/novaworld_common/Dockerfile', 'apps/serve/Dockerfile'):
            for target in dockerfile_build_targets(ROOT / dockerfile):
                directory = target.removeprefix('opennova_')
                binary = 'opennova-' + directory.replace('_', '-')
                self.assertIn(f'apps/{directory}/{binary}', BINARIES)

    def test_image_consumers_require_unit_validated_same_run_binaries(self):
        for name in ('novaworld-images', 'serve-image'):
            text = (WORKFLOWS / f'{name}.yml').read_text(encoding='utf-8')
            self.assertEqual(text.count('service_bundle.py unpack'), 2)
            self.assertEqual(text.count('--require-service-tests'), 2)
            self.assertEqual(text.count('--run-attempt "$GITHUB_RUN_ATTEMPT"'), 2)
            self.assertIn("if: inputs.service-artifact == ''", text)
            self.assertIn("github.event_name == 'push' || github.event_name == 'workflow_dispatch'", text)

    def test_portal_dist_is_consumed_without_disabling_developer_builds(self):
        bake = (ROOT / 'apps/novaworld_common/docker-bake.hcl').read_text(encoding='utf-8')
        self.assertIn('contexts   = PORTAL_CONTEXT != "" ? { build = PORTAL_CONTEXT } : {}', bake)
        dockerfile = (ROOT / 'web/Dockerfile').read_text(encoding='utf-8')
        self.assertIn('RUN npm ci', dockerfile)
        self.assertIn('RUN npm run build', dockerfile)


if __name__ == '__main__':
    unittest.main()
