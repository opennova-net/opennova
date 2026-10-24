"""Package boundary regressions: tracked data, executable modes and smoke isolation.

Run with python scripts/test_package_godot.py. Actual exports and packaged boots
are performed by package_godot.py itself on each platform's packaging CI job.
"""

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import stat
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import package_godot as package


class PackageChecks(unittest.TestCase):
    def setUp(self):
        self.enterContext(redirect_stdout(io.StringIO()))
        self.temporary = tempfile.TemporaryDirectory(prefix="opennova-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, name, data=b"data"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_only_tracked_files_are_copied_and_lfs_pointers_fail(self):
        repo = self.root / "repo"
        tracked = self.write("repo/assets/model with spaces.3di")
        self.write("repo/assets/private.sav", b"local profile")
        subprocess.run(["git", "init", "--quiet", repo], check=True)
        subprocess.run(["git", "-C", repo, "add", "assets/model with spaces.3di"], check=True)
        staged = self.root / "stage"
        package.copy_tracked_project(repo, "assets", staged)
        self.assertEqual(package.stage_files(staged), {"model with spaces.3di"})
        tracked.write_bytes(package.LFS_POINTER + b"\noid sha256:not-pulled\n")
        with self.assertRaisesRegex(package.PackageError, "unpulled LFS pointer"):
            package.copy_tracked_project(repo, "assets", self.root / "lfs-stage")

    def test_windows_version_is_padded_and_restored_after_failure(self):
        original = b'application/file_version=""\r\napplication/product_version=""\r\n'
        presets = self.write("export_presets.cfg", original)
        with self.assertRaisesRegex(RuntimeError, "export failed"):
            with package.windows_version(presets, "0.0.19", True):
                self.assertEqual(presets.read_bytes().count(b'="0.0.19.0"'), 2)
                raise RuntimeError("export failed")
        self.assertEqual(presets.read_bytes(), original)
        with package.windows_version(presets, "0.0.19", False):
            self.assertEqual(presets.read_bytes(), original)

    def test_archives_keep_the_product_layout_without_runner_identity(self):
        for platform_name in ("windows", "linux"):
            with self.subTest(platform=platform_name):
                target = package.Target(platform_name)
                root = self.root / platform_name
                stage = root / "stage"
                name = f"editor/{target.executable('opennova-editor')}"
                executable = self.write(f"{platform_name}/stage/{name}", b"native executable")
                executable.chmod(0o755)
                self.write(f"{platform_name}/stage/runtime/{target.executable('opennova')}")
                expected = package.stage_files(stage)
                archive = root / ("package" + target.archive_suffix)
                package.write_archive(stage, archive, target, expected)
                if platform_name == "linux":
                    with tarfile.open(archive) as packed:
                        for member in packed.getmembers():
                            self.assertEqual((member.uid, member.gid, member.uname, member.gname), (0, 0, "", ""))
                        if os.name != "nt":
                            self.assertTrue(packed.getmember(name).mode & stat.S_IXUSR)
                else:
                    with zipfile.ZipFile(archive) as packed:
                        self.assertEqual(set(packed.namelist()), expected)
                extracted = root / "extracted"
                package.extract_archive(archive, extracted, target, expected)
                self.assertEqual((extracted / name).read_bytes(), b"native executable")
                if platform_name == "linux" and os.name != "nt":
                    self.assertTrue(os.access(extracted / name, os.X_OK))

    def test_extra_save_or_empty_directory_cannot_be_archived(self):
        stage = self.root / "stage"
        self.write("stage/opennova")
        self.write("stage/player.sav")
        with self.assertRaisesRegex(package.PackageError, "player.sav"):
            package.write_archive(stage, self.root / "bad.zip", package.Target("windows"), {"opennova"})
        (stage / "player.sav").unlink()
        (stage / "profile").mkdir()
        with self.assertRaisesRegex(package.PackageError, "profile"):
            package.assert_stage_holds_only(stage, {"opennova"})

    def test_failed_extracted_smoke_does_not_publish_or_overwrite_an_archive(self):
        stage = self.root / "stage"
        self.write("stage/opennova.exe")
        archive = self.write("game.zip", b"previous verified package")

        def writes_a_save(extracted, target):
            self.assertNotEqual(extracted, stage)
            self.assertEqual((extracted / target.executable("opennova")).read_bytes(), b"data")
            (extracted / "weapon.sav").write_text("packager profile", encoding="utf-8")

        with self.assertRaisesRegex(package.PackageError, "weapon.sav"):
            package.publish_archive(stage, archive, package.Target("windows"), writes_a_save)
        self.assertEqual(archive.read_bytes(), b"previous verified package")
        self.assertFalse((self.root / ".game.zip.unverified").exists())
        self.assertEqual(package.stage_files(stage), {"opennova.exe"})

    def test_archive_paths_and_links_are_rejected(self):
        archive = self.root / "bad.tar.gz"
        with tarfile.open(archive, "w:gz") as packed:
            link = tarfile.TarInfo("opennova")
            link.type = tarfile.SYMTYPE
            link.linkname = "../outside"
            packed.addfile(link)
        with self.assertRaisesRegex(package.PackageError, "not a regular file"):
            package.extract_archive(archive, self.root / "out", package.Target("linux"), {"opennova"})
        with zipfile.ZipFile(self.root / "bad.zip", "w") as packed:
            packed.writestr("../outside", "data")
        with self.assertRaisesRegex(package.PackageError, "Unexpected archive member"):
            package.extract_archive(self.root / "bad.zip", self.root / "out", package.Target("windows"), {"opennova"})

    def test_boot_isolates_profile_and_working_directory(self):
        previous = os.environ.copy()
        executable = self.write("opennova.exe")
        with patch.object(package, "run", return_value="boot completed") as run:
            self.assertEqual(package.boot(executable, ["--resource-dir"], 2), "boot completed")
        command = run.call_args.args[0]
        arguments = run.call_args.kwargs
        profile = arguments["cwd"]
        self.assertEqual(arguments["expected"], 2)
        self.assertEqual(command[-3:], ["--working-dir", profile, "--resource-dir"])
        for variable in ("APPDATA", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            self.assertEqual(arguments["env"][variable], profile)
        self.assertNotEqual(Path(profile), executable.parent)
        self.assertFalse(Path(profile).exists())
        self.assertEqual(os.environ, previous)

    def test_load_errors_fail_even_after_successful_process_exit(self):
        for error in ("SCRIPT ERROR: Parse Error", "Can't open dynamic library", "Failed loading scene",
                      "No loader found for resource", "GDExtension dynamic library not found"):
            with self.subTest(error=error), self.assertRaises(package.PackageError):
                package.check_load_errors(error, "successful export")

    def test_linux_dependency_check_rejects_unresolved_libraries(self):
        header = b"\x7fELF\x02\x01" + bytes(12) + b"\x3e\x00"
        self.write("stage/libopennova.so", header)
        self.write("stage/resource.pff")
        with patch.object(package, "run", side_effect=["GLIBC_2.39", "libstdc++.so.6 => not found\n"]):
            with self.assertRaisesRegex(package.PackageError, "Unresolved Linux native dependencies"):
                package.check_linux_dependencies(self.root / "stage")

    def test_linux_native_architecture_and_glibc_baseline_are_enforced(self):
        arm_header = b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00"
        library = self.write("stage/libopennova.so", arm_header)
        with self.assertRaisesRegex(package.PackageError, "not an x86_64 ELF"):
            package.check_linux_dependencies(self.root / "stage")
        library.write_bytes(arm_header[:-2] + b"\x3e\x00")
        with patch.object(package, "run", return_value="Name: GLIBC_2.40"):
            with self.assertRaisesRegex(package.PackageError, "glibc newer than Ubuntu 24.04"):
                package.check_linux_dependencies(self.root / "stage")


if __name__ == "__main__":
    unittest.main()
