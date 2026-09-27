"""Regression tests for suite isolation and missing-coverage detection."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("test_suites", Path(__file__).with_name("test_suites.py"))
suites = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suites)


class SuiteChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def report(self, body):
        return self.write("result.xml", "<testsuites><testsuite>" + body + "</testsuite></testsuites>")

    def test_core_and_retail_are_disjoint_and_exclude_windowed(self):
        for name in ["plain", "retail/compat", "retail/windowed/pixel"]:
            self.write("godot/tests/" + name + "_test.gd", "extends GutTest\nfunc test_works():\n\tpass\n")
        core = suites.godot_scripts(self.root, "core")
        retail = suites.godot_scripts(self.root, "retail")
        self.assertEqual(core, ["res://tests/plain_test.gd"])
        self.assertEqual(retail, ["res://tests/retail/compat_test.gd"])
        self.assertEqual(suites.godot_scripts(self.root, "all"), core + retail)
        self.assertEqual(suites.godot_scripts(self.root, "retail", windowed=True),
                         ["res://tests/retail/windowed/pixel_test.gd"])

    def test_missing_native_test_and_unexpected_test_fail(self):
        report = self.report('<testcase name="unexpected"/>')
        errors = suites.check_report(report, ["required"], godot=False, suite="core")
        self.assertIn("not in report: required", errors)
        self.assertIn("unexpected test: unexpected", errors)

    def test_missing_method_is_not_hidden_by_a_passing_sibling(self):
        report = self.report('<testcase classname="tests/example_test.gd" name="test_first"/>')
        errors = suites.check_report(report, ["res://tests/example_test.gd"], godot=True,
            suite="core", methods={"res://tests/example_test.gd": ["test_first", "test_second"]})
        self.assertEqual(errors, ["method not in report: tests/example_test.gd:test_second"])

    def test_gut_classname_without_resource_prefix_matches(self):
        report = self.report('<testcase classname="tests/example_test.gd" name="test_first"/>')
        self.assertEqual(suites.check_report(report, ["res://tests/example_test.gd"],
            godot=True, suite="core"), [])

    def test_retail_skip_and_skipped_leg_fail(self):
        report = self.report('<testcase name="whole"><skipped/></testcase>'
            '<testcase name="mixed"><system-out>' + "padding " * 2000 +
            'SKIP-LEG: needs data</system-out></testcase>')
        errors = suites.check_report(report, ["whole", "mixed"], godot=False, suite="retail")
        self.assertTrue(any("retail test skipped" in e for e in errors))
        self.assertTrue(any("retail coverage not exercised" in e for e in errors))

    def test_accidental_gate_in_core_fails(self):
        report = self.report('<testcase name="gate"><system-out>SKIP: needs a game</system-out></testcase>')
        self.assertTrue(suites.check_report(report, ["gate"], godot=False, suite="core"))

    def test_graphics_pending_is_allowed_in_core_but_retail_pending_is_not(self):
        report = self.report('<testcase classname="tests/pixel_test.gd" name="test_pixel">'
            '<skipped>RenderingDevice unavailable</skipped></testcase>')
        self.assertEqual(suites.check_report(report, ["res://tests/pixel_test.gd"],
            godot=True, suite="core"), [])
        report = self.report('<testcase classname="tests/pixel_test.gd" name="test_pixel">'
            '<skipped>OPENNOVA_JO_ASSETS required</skipped></testcase>')
        self.assertTrue(suites.check_report(report, ["res://tests/pixel_test.gd"],
            godot=True, suite="core"))

    def test_empty_run_fails(self):
        report = self.report("")
        self.assertTrue(suites.check_report(report, [], godot=False, suite="core"))

    def test_retail_requires_both_existing_directories(self):
        with patch.dict(suites.os.environ, {}, clear=True):
            self.assertEqual(len(suites.check_roots()), 2)
        ordinary_file = self.write("not-a-directory", "fixture")
        with patch.dict(suites.os.environ, {"OPENNOVA_JO_DIR": str(self.root),
                "OPENNOVA_JO_ASSETS": str(ordinary_file)}, clear=True):
            self.assertEqual(suites.check_roots(), ["OPENNOVA_JO_ASSETS must name an existing directory"])
        with patch.dict(suites.os.environ, {name: str(self.root) for name in suites.ROOTS}, clear=True):
            self.assertEqual(suites.check_roots(), [])

    def test_parse_errors_and_silent_collection_drop_fail(self):
        log = self.write("gut.log", "SCRIPT ERROR: Parse Error: missing class\n"
            "Ignoring script res://tests/lost_test.gd because it does not extend GutTest\n")
        self.assertEqual(len(suites.check_log(log, "core")), 2)

    def test_all_only_accepts_explicit_retail_script_skips(self):
        log = self.write("gut.log", "res://tests/retail/data_test.gd\n"
            "- [Script skipped]: OPENNOVA_JO_ASSETS is required\n"
            "res://tests/core_test.gd\n- [Script skipped]: OPENNOVA_JO_ASSETS is required\n")
        self.assertEqual(suites.skipped_retail_scripts(log), {"tests/retail/data_test.gd"})

    def test_core_layout_rejects_direct_and_helper_retail_access(self):
        self.write("godot/tests/one_test.gd", "func test_one():\n\tRetailData.assets()\n")
        self.write("godot/tests/two_test.gd", "func test_two():\n\tPresenterFixture.stage(self, 'x', {})\n")
        self.assertEqual(len(suites.layout_errors(self.root)), 2)

    def test_ctest_inventory_uses_labels_in_both_directions(self):
        with patch.object(suites.subprocess, "check_output", return_value=json.dumps(
                {"tests": [{"name": "one"}]})) as command:
            self.assertEqual(suites.ctest_inventory(self.root, "core"), ["one"])
            self.assertEqual(command.call_args.args[0][-2:], ["-LE", "^retail$"])
            suites.ctest_inventory(self.root, "retail")
            self.assertEqual(command.call_args.args[0][-2:], ["-L", "^retail$"])

    def test_migration_cannot_lose_an_original_method(self):
        self.write("scripts/ci/retail_migration.json", json.dumps([{
            "original_script": "godot/tests/example_test.gd",
            "core_script": "godot/tests/example_test.gd", "core_methods": ["test_original"],
            "retail_script": None, "retail_methods": []}]))
        errors = suites.layout_errors(self.root)
        self.assertEqual(errors, ["migrated method missing: godot/tests/example_test.gd:test_original"])
        self.write("godot/tests/example_test.gd", "extends GutTest\nfunc test_original():\n\tpass\n")
        self.assertEqual(suites.layout_errors(self.root), [])


if __name__ == "__main__":
    unittest.main()
