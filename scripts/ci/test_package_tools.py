"""Prebuilt package tools keep archive contents and standalone entrypoints stable."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ("3di", "extract", "lan-probe", "parity", "serve", "update-tool", "wire")
STAMP = ("-DOPENNOVA_VERSION=1.2.3.456", "-DOPENNOVA_STAGING=ON",
         "-DOPENNOVA_COMMIT=abc1234", "-DOPENNOVA_BUILD_TIME=1234567890")


@unittest.skipUnless(shutil.which("bash"), "Bash is needed for package entrypoints")
class PrebuiltPackages(unittest.TestCase):
    def test_shared_binaries_are_packaged_without_invoking_cmake(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            tools = base / "tools with spaces"
            tools.mkdir()
            for name in TOOLS:
                (tools / f"opennova-{name}.exe").write_bytes(name.encode())
            # A configure/build would fail this test; the supplied binaries must
            # be the actual files in both archives, not rebuilt substitutes.
            bin_dir = base / "bin"
            bin_dir.mkdir()
            (bin_dir / "cmake").write_text("#!/bin/sh\nexit 99\n")
            (bin_dir / "python").write_text(f"#!/bin/sh\nexec {shlex.quote(Path(sys.executable).as_posix())} \"$@\"\n")
            for path in bin_dir.iterdir():
                path.chmod(0o755)
            env = {**os.environ, "PATH": str(bin_dir) + os.pathsep + os.environ.get("PATH", "")}
            apps_zip = base / "tools.zip"
            self.run_script("package_apps.sh", apps_zip, tools, *STAMP, env=env)
            with zipfile.ZipFile(apps_zip) as archive:
                self.assertEqual(set(archive.namelist()),
                                 {f"opennova-{name}.exe" for name in TOOLS} | {"README.txt", "example.host"})
                for name in TOOLS:
                    self.assertEqual(archive.read(f"opennova-{name}.exe"), name.encode())
            blender_zip = base / "blender.zip"
            self.run_script("package_blender_addon.sh", blender_zip, "v1.2.3", tools, env=env)
            with zipfile.ZipFile(blender_zip) as archive:
                self.assertEqual(archive.read("opennova_3di/bin/opennova-3di.exe"), b"3di")
                self.assertIn('version = "1.2.3"', archive.read("opennova_3di/blender_manifest.toml").decode())
            (tools / "opennova-3di.exe").unlink()
            result = self.run_script("package_blender_addon.sh", base / "missing.zip", "", tools,
                                     env=env, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((base / "missing.zip").exists())

    def test_standalone_and_shared_builds_forward_the_one_stamp(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            # Stop at configure after printing each argument. No native output
            # is written, and preserving argument boundaries is observable.
            cmake = base / "cmake"
            cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\nexit 42\n')
            cmake.chmod(0o755)
            env = {**os.environ, "PATH": str(base) + os.pathsep + os.environ.get("PATH", "")}
            for name, args in (
                ("build_package_tools.sh", ("--tools", "--godot")),
                ("package_apps.sh", (base / "standalone.zip",)),
                ("package_apps.sh", (base / "empty-prebuilt.zip", "")),
            ):
                with self.subTest(script=name, args=args):
                    result = self.run_script(name, *args, *STAMP, env=env, check=False)
                    self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
                    configure = result.stdout.splitlines()
                    for argument in STAMP:
                        self.assertEqual(configure.count(argument), 1, configure)
                    self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", configure)

    def run_script(self, name, *args, env, check=True):
        result = subprocess.run(["bash", (ROOT / "scripts" / name).as_posix(),
                                 *(arg.as_posix() if isinstance(arg, Path) else arg for arg in args)],
                                cwd=ROOT, env=env, capture_output=True, text=True)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result


if __name__ == "__main__":
    unittest.main()
