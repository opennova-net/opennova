"""Regression tests for the suite's selection and missing-coverage detection."""
import importlib.util
import json
import re
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

    def test_windowed_scripts_are_their_own_selection(self):
        for name in ["plain", "net/wire", "windowed/raster"]:
            self.write("godot/tests/" + name + "_test.gd", "extends GutTest\nfunc test_works():\n\tpass\n")
        self.assertEqual(suites.godot_scripts(self.root),
                         ["res://tests/net/wire_test.gd", "res://tests/plain_test.gd"])
        self.assertEqual(suites.godot_scripts(self.root, windowed=True),
                         ["res://tests/windowed/raster_test.gd"])

    def test_windowed_run_fails_on_a_pending_test(self):
        report = self.report('<testcase classname="tests/windowed/raster_test.gd" name="test_pixel">'
            '<skipped>RenderingDevice unavailable</skipped></testcase>')
        self.assertEqual(suites.check_report(report, ["res://tests/windowed/raster_test.gd"], godot=True), [])
        self.assertEqual(suites.check_report(report, ["res://tests/windowed/raster_test.gd"],
            godot=True, windowed=True),
            ["windowed test skipped: tests/windowed/raster_test.gd:test_pixel"])
        log = self.write("gut.log", "res://tests/windowed/raster_test.gd\n"
            "- [Pending]: RenderingDevice unavailable under this Godot renderer\n")
        self.assertEqual(suites.check_log(log), [])
        self.assertEqual(len(suites.check_log(log, windowed=True)), 1)

    def test_layout_rejects_retail_reads_under_either_test_tree(self):
        self.write("godot/tests/windowed/raster_test.gd", "func test_one():\n\tRetailData.assets()\n")
        self.write("godot/tests/env_test.gd", 'func test_two():\n\tOS.get_environment("OPENNOVA_JO_DIR")\n')
        self.write("tests/world/walk_test.cpp", '#include "common/retail_paths.h"\n')
        self.write("tests/mission/boot_test.cpp", '#include "common/retail_mission_files.h"\n')
        self.write("tests/plain_test.cpp", "int main() { return 0; }\n")
        self.write("tests/fixtures/blob.bin", "OPENNOVA_JO_ASSETS")
        self.assertEqual(suites.layout_errors(self.root), [
            "test reads retail data (retail_mission_files): tests/mission/boot_test.cpp",
            "test reads retail data (retail_paths): tests/world/walk_test.cpp",
            "test reads retail data (OPENNOVA_JO_): godot/tests/env_test.gd",
            "test reads retail data (RetailData): godot/tests/windowed/raster_test.gd",
        ])

    def test_missing_native_test_and_unexpected_test_fail(self):
        report = self.report('<testcase name="unexpected"/>')
        errors = suites.check_report(report, ["required"], godot=False)
        self.assertIn("not in report: required", errors)
        self.assertIn("unexpected test: unexpected", errors)

    def test_missing_method_is_not_hidden_by_a_passing_sibling(self):
        report = self.report('<testcase classname="tests/example_test.gd" name="test_first"/>')
        errors = suites.check_report(report, ["res://tests/example_test.gd"], godot=True,
            methods={"res://tests/example_test.gd": ["test_first", "test_second"]})
        self.assertEqual(errors, ["method not in report: tests/example_test.gd:test_second"])

    def test_gut_classname_without_resource_prefix_matches(self):
        report = self.report('<testcase classname="tests/example_test.gd" name="test_first"/>')
        self.assertEqual(suites.check_report(report, ["res://tests/example_test.gd"], godot=True), [])

    def test_a_data_skip_fails(self):
        report = self.report('<testcase name="pg"><system-out>' + "padding " * 2000 +
            'SKIP: needs a PostgreSQL server</system-out></testcase>')
        self.assertEqual(suites.check_report(report, ["pg"], godot=False), ["test skipped: :pg"])

    def test_graphics_pending_is_allowed_headless(self):
        report = self.report('<testcase classname="tests/pixel_test.gd" name="test_pixel">'
            '<skipped>RenderingDevice unavailable</skipped></testcase>')
        self.assertEqual(suites.check_report(report, ["res://tests/pixel_test.gd"], godot=True), [])

    def test_empty_run_fails(self):
        report = self.report("")
        self.assertTrue(suites.check_report(report, [], godot=False))

    def test_parse_errors_and_silent_collection_drop_fail(self):
        log = self.write("gut.log", "SCRIPT ERROR: Parse Error: missing class\n"
            "Ignoring script res://tests/lost_test.gd because it does not extend GutTest\n")
        self.assertEqual(len(suites.check_log(log)), 2)

    def test_ctest_inventory_selects_by_shard(self):
        with patch.object(suites.subprocess, "check_output", return_value=json.dumps(
                {"tests": [{"name": "one"}]})) as command:
            self.assertEqual(suites.ctest_inventory(self.root), ["one"])
            self.assertEqual(command.call_args.args[0][-1], "--show-only=json-v1")
            suites.ctest_inventory(self.root, "editor")
            self.assertEqual(command.call_args.args[0][-2:], suites.SHARDS["editor"])

    def test_ctest_shards_partition_the_suite(self):
        def selects(args, labels):
            # ctest: -L keeps a test with a label matching, -LE drops one.
            picked = True
            for flag, pattern in zip(args[::2], args[1::2]):
                hit = any(re.search(pattern, label) for label in labels)
                picked = picked and (hit if flag == "-L" else not hit)
            return picked

        self.assertEqual(suites.ctest_selection(), [])
        for labels in ([], ["editor"], ["redis"], ["editor", "redis"], ["other"]):
            shards = [name for name in suites.SHARDS if selects(suites.ctest_selection(name), labels)]
            self.assertEqual(len(shards), 1, labels)

    def test_godot_shards_partition_the_suite(self):
        for name in ["plain", "net/wire", "authoring/editor", "game/shell", "mcp/server", "probes/probe",
                     "audio/mix", "windowed/raster"]:
            self.write("godot/tests/" + name + "_test.gd", "extends GutTest\nfunc test_works():\n\tpass\n")
        scripts = suites.godot_scripts(self.root)
        shards = {name: suites.godot_scripts(self.root, shard=name) for name in suites.GODOT_SHARDS}
        self.assertEqual(sorted(shards["shells"] + shards["world"]), scripts)
        self.assertFalse(set(shards["shells"]) & set(shards["world"]))
        self.assertEqual(shards["shells"], ["res://tests/authoring/editor_test.gd", "res://tests/game/shell_test.gd",
                                            "res://tests/mcp/server_test.gd", "res://tests/probes/probe_test.gd"])
        with self.assertRaises(ValueError):
            suites.godot_scripts(self.root, shard="editor")


if __name__ == "__main__":
    unittest.main()
