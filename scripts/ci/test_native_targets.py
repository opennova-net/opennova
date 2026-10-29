"""Exercise the native shard build graph, including tests registered after editor/."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake is needed to check build target closure")
class NativeTargets(unittest.TestCase):
    def test_each_shard_builds_its_tests_and_rest_builds_app_commands(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source"
            source.mkdir()
            (source / "main.c").write_text("int main(void) { return 0; }\n")
            files = {
                "CMakeLists.txt": f'''cmake_minimum_required(VERSION 3.16)
project(shards C)
enable_testing()
add_subdirectory(apps/tool)
add_subdirectory(tests)
include("{(ROOT / 'scripts/ci/native_targets.cmake').as_posix()}")
''',
                "apps/tool/CMakeLists.txt": '''add_executable(tool ${PROJECT_SOURCE_DIR}/main.c)
''',
                "tests/CMakeLists.txt": '''add_executable(before ${PROJECT_SOURCE_DIR}/main.c)
add_test(NAME before COMMAND before)
add_subdirectory(editor)
add_executable(after ${PROJECT_SOURCE_DIR}/main.c)
add_test(NAME after COMMAND after)
add_test(NAME cli COMMAND tool)
''',
                "tests/editor/CMakeLists.txt": '''add_executable(editor ${PROJECT_SOURCE_DIR}/main.c)
add_test(NAME editor COMMAND editor)
set_property(TEST editor PROPERTY LABELS editor)
''',
            }
            for name, content in files.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            selections = {}
            for shard, expected in (("editor", {"editor"}), ("rest", {"before", "after", "cli"})):
                build = Path(temp) / shard
                self.run_command("cmake", "-S", str(source), "-B", str(build))
                self.run_command("cmake", "--build", str(build), "--config", "Release",
                                 "--target", f"opennova_tests_{shard}")
                select = ["-L" if shard == "editor" else "-LE", "^editor$"]
                inventory = json.loads(self.run_command(
                    "ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1", *select))
                selections[shard] = {row["name"] for row in inventory["tests"]}
                self.assertEqual(selections[shard], expected)
                for row in inventory["tests"]:
                    self.assertTrue(Path(row["command"][0]).is_file(), row)
                self.run_command("ctest", "--test-dir", str(build), "-C", "Release",
                                 "--output-on-failure", *select)
                excluded = "before" if shard == "editor" else "editor"
                self.assertFalse(any(path.stem == excluded for path in build.rglob("*")
                                     if path.is_file() and path.suffix in ("", ".exe")))
            self.assertFalse(selections["editor"] & selections["rest"])
            self.assertEqual(selections["editor"] | selections["rest"], {"before", "after", "cli", "editor"})

    def run_command(self, *args):
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout


if __name__ == "__main__":
    unittest.main()
