import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import changes


class ComponentSelection(unittest.TestCase):
    def selected(self, *paths):
        return {key for key, enabled in changes.classify(list(paths)).items() if enabled}

    def test_documentation_needs_only_always_running_gates(self):
        self.assertEqual(self.selected('docs', 'README.md', 'engine/README.md'), set())

    def test_component_cases(self):
        cases = {
            'godot/game/menu.gd': {'godot'},
            'godot/shaders/object/pipeline_manifest.json': {'godot', 'native'},
            'third_party/godot-cpp': {'godot'},
            'tests/editor/session_test.cpp': {'native'},
            'tests/runtime/world_test.cpp': {'native'},
            'tools/blender/opennova_3di/model.py': {'blender'},
            'tests/blender/test_model.py': {'blender'},
            'web/src/App.vue': {'web', 'stack'},
            'backend/migrations/001.sql': changes.SERVICE,
            'apps/novaworld_web/main.cpp': changes.SERVICE,
            'infra/ci/runners.tf': {'infra'},
            'deploy/compose/docker-compose.yml': changes.SERVICE | {'infra'},
            'scripts/lint/test_fixture_lint.py': set(),
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), expected)

    def test_shared_and_unknown_inputs_select_everything(self):
        for path in ('engine/net/codec.cpp', 'apps/common/sockets.h', 'CMakeLists.txt',
                     '.github/workflows/ci.yml', 'fixtures/new.bin', 'tests/fixtures/mint.cpp',
                     'scripts/build.sh', 'new-component/file.txt', '../web/a.ts'):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), set(changes.COMPONENTS))
        self.assertTrue(all(changes.classify(None).values()))
        self.assertTrue(all(changes.classify([]).values()))

    def test_stack_harness_changes_execute_the_stack_without_a_ctest_row(self):
        for path in ('tests/novaworld/stack_smoke.cpp', 'tests/novaworld/lobby_client.h',
                     'tests/serve/serve_test_support.h', 'tests/novaworld/service_tests.cmake'):
            with self.subTest(path=path):
                self.assertEqual(self.selected(path), {'native', 'web', 'stack'})

    def test_mixed_paths_union_the_components(self):
        self.assertEqual(self.selected('web/src/App.vue', 'godot/game/menu.gd'), {'web', 'stack', 'godot'})

    def test_machine_and_entrypoint_also_run_infra(self):
        self.assertIn('infra', self.selected('apps/novaworld_common/hosted/cloud/machine/boot.sh'))
        self.assertEqual(self.selected('web/docker-entrypoint.sh'), {'web', 'stack', 'infra'})

    @patch('changes.subprocess.run')
    def test_paginated_rename_includes_old_component(self, run):
        run.return_value.stdout = json.dumps([
            [{'filename': 'web/new.ts', 'previous_filename': 'engine/old.cpp'}],
            [{'filename': 'README.md'}],
        ])
        paths = changes.changed_paths({'number': 12, 'pull_request': {'changed_files': 2}}, 'owner/repo')
        self.assertIn('engine/old.cpp', paths)
        self.assertTrue(all(changes.classify(paths).values()))
        self.assertIn('--paginate', run.call_args.args[0])

    @patch('changes.subprocess.run')
    def test_truncated_api_is_never_a_partial_selection(self, run):
        run.return_value.stdout = json.dumps([[{'filename': 'README.md'}]])
        with self.assertRaises(ValueError):
            changes.changed_paths({'number': 12, 'pull_request': {'changed_files': 3001}}, 'owner/repo')

    def test_manual_and_failed_pr_queries_run_full_graph(self):
        with tempfile.TemporaryDirectory() as directory:
            event = Path(directory) / 'event.json'
            event.write_text('{}', encoding='utf-8')
            for kind in ('push', 'workflow_dispatch', 'pull_request'):
                output = Path(directory) / kind
                changes.main(['--event', kind, '--event-path', str(event),
                              '--repository', 'owner/repo', '--github-output', str(output)])
                self.assertTrue(all(f'{name}=true' in output.read_text() for name in changes.COMPONENTS))


if __name__ == '__main__':
    unittest.main()
