"""package_release.py's commands for each kind of product, with the tools stood in for."""

import argparse
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import package_release


class PackageReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.calls = []

    def tearDown(self):
        self.temp.cleanup()

    def project(self, name, expansion=None):
        folder = self.root / name
        folder.mkdir()
        document = {"schema_version": 2, "project_id": name, "title": name}
        if expansion:
            document["expansion"] = {"name": expansion, "builds_on": "", "base_project": "../BASE"}
            (self.root / "BASE").mkdir(exist_ok=True)
        (folder / "project.opennova").write_text(json.dumps(document), encoding="utf-8")
        return folder

    def fake(self, args, **kwargs):
        args = [str(a) for a in args]
        self.calls.append(args)
        if "export" in args and "--out" in args:
            Path(args[args.index("--out") + 1]).mkdir(parents=True, exist_ok=True)
        if "build" in args:
            Path(args[args.index("--out") + 1]).write_bytes(b"1SFS")
        return subprocess.CompletedProcess(args, 0, b"product: stand-in\n", b"")

    def package(self, **options):
        settings = {
            "project_tool": Path("opennova-project"),
            "update_tool": Path("opennova-update-tool"),
            "install": None,
            "previous": None,
            "out": self.root / "RELEASES",
        }
        settings.update(options)
        args = argparse.Namespace(**settings)
        with (
            mock.patch.object(package_release, "run", self.fake),
            mock.patch.object(package_release, "quiet", self.fake),
        ):
            return package_release.build(args)

    def build_call(self):
        return next(call for call in self.calls if "build" in call)

    def test_retail_patch_diffs_a_pristine_import(self):
        project = self.project("JO")
        install = self.root / "install"
        install.mkdir()
        package = self.package(product="JO", project=project, version="1.7.5.8", install=install)
        self.assertEqual(package.name, "JO-1.7.5.8.sfs")
        self.assertTrue((package.parent / "JO-1.7.5.8.txt").is_file())
        imports = [call for call in self.calls if "import" in call]
        self.assertEqual(len(imports), 1)
        self.assertIn("--all", imports[0])
        build = self.build_call()
        self.assertTrue(build[build.index("--from") + 1].endswith("pristine-export"))
        self.assertEqual(build[build.index("--base") + 1], "JOPTR.CMD")
        self.assertEqual(build[build.index("--exe") + 1], "Jointops.exe")
        self.assertEqual(build[build.index("--version") + 1], "1.7.5.8")
        self.assertEqual(build[build.index("--product") + 1], "Joint Operations")
        self.assertNotIn("--full", build)

    def test_retail_expansion_stands_on_a_pristine_base(self):
        project = self.project("JOX01", expansion="jox01")
        install = self.root / "install"
        install.mkdir()
        self.package(product="JOX01", project=project, version="1.3.0.1", install=install)
        imports = [call for call in self.calls if "import" in call]
        self.assertEqual(len(imports), 2)
        self.assertIn("--expansion", imports[1])
        build = self.build_call()
        self.assertEqual(build[build.index("--expansion") + 1], "jox01=JOEPTR.CMD")
        self.assertNotIn("--base", build)
        self.assertNotIn("--exe", build)

    def test_native_package_is_full_with_its_predecessor(self):
        project = self.project("OPENNOVA")
        previous = self.root / "OPENNOVA-0.1.0.sfs"
        previous.write_bytes(b"1SFS")
        self.package(product="OPENNOVA", project=project, version="0.2.0", previous=previous)
        self.assertFalse(any("import" in call for call in self.calls))
        build = self.build_call()
        self.assertTrue(build[build.index("--from") + 1].endswith("empty"))
        self.assertIn("--full", build)
        self.assertEqual(Path(build[build.index("--previous") + 1]), previous.resolve())
        self.assertEqual(build[build.index("--base") + 1], "OPENNOVA.CMD")

    def test_native_expansion_exports_its_base_first(self):
        project = self.project("ONJO1", expansion="onjo1")
        self.package(product="ONJO1", project=project, version="0.2.0")
        exports = [call for call in self.calls if "export" in call]
        self.assertTrue(exports[0][-1].endswith("BASE"))
        self.assertEqual(
            self.build_call()[self.build_call().index("--expansion") + 1], "onjo1=ONJO1.CMD"
        )

    def test_refusals(self):
        project = self.project("JO")
        with self.assertRaisesRegex(ValueError, "--install"):
            self.package(product="JO", project=project, version="1.0")
        with self.assertRaisesRegex(ValueError, "no --install"):
            self.package(product="OPENNOVA", project=project, version="1.0", install=self.root)
        with self.assertRaisesRegex(ValueError, "no OpenNova Editor project"):
            self.package(product="OPENNOVA", project=self.root / "missing", version="1.0")


if __name__ == "__main__":
    unittest.main()
