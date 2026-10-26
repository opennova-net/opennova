"""stage_downloads.py: the builds under their stable names, manifest.json, and the refusals."""

from __future__ import annotations

import contextlib
import datetime
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import stage_downloads

NOW = datetime.datetime(2026, 10, 26, 12, 0, 0, tzinfo=datetime.timezone.utc)
COMMIT = "2d23f67" + "0" * 33


class StageDownloadsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "dist"
        self.source.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name: str, text: str) -> None:
        (self.source / name).write_text(text, encoding="utf-8")

    def release_assets(self, tag: str) -> None:
        for build in stage_downloads.BUILDS:
            self.write(build.replace(".zip", f"-{tag}.zip"), build)

    def test_stable_names(self):
        self.assertEqual(stage_downloads.stable_name("opennova-game-windows-v0.0.21.zip"), "opennova-game-windows.zip")
        self.assertEqual(stage_downloads.stable_name("dfx2-game-windows-v0.0.20.zip"), "dfx2-game-windows.zip")
        self.assertEqual(stage_downloads.stable_name("opennova-apps-windows.zip"), "opennova-apps-windows.zip")
        self.assertEqual(stage_downloads.stable_name("opennova-runtime-windows-v0.0.21.sfs"), "opennova-runtime-windows-v0.0.21.sfs")

    def test_versions_from_names(self):
        self.assertEqual(stage_downloads.version_of("opennova-game-windows-v0.0.20.zip"), "0.0.20")
        self.assertEqual(stage_downloads.version_of("opennova-switcher-windows-v1.zip"), "1")
        self.assertIsNone(stage_downloads.version_of("opennova-apps-windows.zip"))
        self.assertIsNone(stage_downloads.version_of("opennova-runtime-windows-v0.0.21.sfs"))

    def test_release_assets_staged_with_manifest(self):
        self.release_assets("v0.0.21")
        self.write("opennova-runtime-windows-v0.0.21.sfs", "not a download")
        self.write("opennova-switcher-windows-v0.0.21.zip.sha256", "not a download")
        out = self.root / "stage"
        manifest = stage_downloads.stage(self.source, out, "v0.0.21", "https://example.com/v0.0.21", COMMIT, "release", NOW)
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted(stage_downloads.BUILDS + ("manifest.json",)))
        on_disk = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(on_disk, manifest)
        self.assertEqual(manifest["version"], "v0.0.21")
        self.assertEqual(manifest["published"], "2026-10-26T12:00:00Z")
        self.assertEqual(manifest["source"], "https://example.com/v0.0.21")
        self.assertEqual(manifest["commit"], COMMIT)
        self.assertEqual(manifest["build"], "release")
        self.assertEqual(manifest["talks_to"], "prod")
        game = manifest["files"]["opennova-game-windows.zip"]
        self.assertEqual(game["size"], len("opennova-game-windows.zip"))
        self.assertEqual(game["sha256"], hashlib.sha256(b"opennova-game-windows.zip").hexdigest())
        self.assertEqual({file["version"] for file in manifest["files"].values()}, {"0.0.21"})

    def test_ci_artifacts_mix_versioned_and_plain_names(self):
        for build in stage_downloads.BUILDS:
            versioned = "game" in build or "editor" in build
            self.write(build.replace(".zip", "-v0.0.20.zip") if versioned else build, build)
        manifest = stage_downloads.stage(self.source, self.root / "stage", "PR #1 (2d23f67)", "run", COMMIT, "debug", NOW)
        self.assertEqual(sorted(manifest["files"]), sorted(stage_downloads.BUILDS))
        self.assertEqual(manifest["build"], "debug")
        self.assertEqual(manifest["talks_to"], "staging")
        # A versioned name's version; a plain name has none.
        self.assertEqual(manifest["files"]["opennova-game-windows.zip"]["version"], "0.0.20")
        self.assertEqual(manifest["files"]["opennova-editor-windows.zip"]["version"], "0.0.20")
        self.assertNotIn("version", manifest["files"]["opennova-apps-windows.zip"])
        self.assertNotIn("version", manifest["files"]["opennova-switcher-windows.zip"])

    def test_a_short_commit_or_another_build_kind_refuses(self):
        self.release_assets("v0.0.21")
        with self.assertRaisesRegex(stage_downloads.StageError, "the commit is a full sha"):
            stage_downloads.stage(self.source, self.root / "stage", "v0.0.21", "x", "2d23f67", "release", NOW)
        with self.assertRaisesRegex(stage_downloads.StageError, "the build is release or debug"):
            stage_downloads.stage(self.source, self.root / "stage", "v0.0.21", "x", COMMIT, "nightly", NOW)
        self.assertFalse((self.root / "stage").exists())

    def test_main_takes_the_commit_and_build(self):
        self.release_assets("v0.0.21")
        out = self.root / "stage"
        with contextlib.redirect_stdout(io.StringIO()):
            code = stage_downloads.main(["--from", str(self.source), "--to", str(out), "--version", "v0.0.21",
                                         "--source", "x", "--commit", COMMIT, "--build", "release"])
        self.assertEqual(code, 0)
        on_disk = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual((on_disk["commit"], on_disk["build"]), (COMMIT, "release"))

    def test_a_missing_build_refuses(self):
        self.release_assets("v0.0.21")
        (self.source / "dfx2-game-windows-v0.0.21.zip").unlink()
        with self.assertRaisesRegex(stage_downloads.StageError, "missing dfx2-game-windows.zip"):
            stage_downloads.stage(self.source, self.root / "stage", "v0.0.21", "x", COMMIT, "release", NOW)

    def test_two_files_of_one_build_refuse(self):
        self.release_assets("v0.0.21")
        self.write("opennova-game-windows-v0.0.20.zip", "older")
        with self.assertRaisesRegex(stage_downloads.StageError, "two files are opennova-game-windows.zip"):
            stage_downloads.stage(self.source, self.root / "stage", "v0.0.21", "x", COMMIT, "release", NOW)


if __name__ == "__main__":
    unittest.main()
