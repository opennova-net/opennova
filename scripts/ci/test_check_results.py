import unittest

from changes import COMPONENTS
from check_results import GROUPS, check


def result_graph(group, *selected):
    outputs = {key: str(key in selected).lower() for key in COMPONENTS}
    needs = {'changes': {'result': 'success', 'outputs': outputs}}
    for job, components in GROUPS[group].items():
        needs[job] = {'result': 'success' if not components or set(components) & set(selected) else 'skipped'}
    return needs


class RequiredChecks(unittest.TestCase):
    def test_full_and_deliberately_scoped_graphs(self):
        for group in GROUPS:
            for selected in ((), COMPONENTS, ('web', 'stack'), ('godot',), ('native',)):
                with self.subTest(group=group, selected=selected):
                    self.assertEqual(check(group, result_graph(group, *selected)), [])

    def test_failed_or_skipped_required_component_is_red(self):
        for group, jobs in GROUPS.items():
            for job in jobs:
                for status in ('failure', 'skipped', 'cancelled'):
                    needs = result_graph(group, *COMPONENTS)
                    needs[job]['result'] = status
                    self.assertTrue(check(group, needs), (group, job, status))

    def test_missing_changes_outputs_never_produce_green(self):
        needs = result_graph('godot')
        del needs['changes']['outputs']['godot']
        self.assertTrue(check('godot', needs))
        self.assertTrue(check('native', {}))

    def test_prerequisite_build_is_expected_for_stack_only(self):
        needs = result_graph('native', 'web', 'stack')
        needs['service-build']['result'] = 'skipped'
        self.assertTrue(check('native', needs))


if __name__ == '__main__':
    unittest.main()
