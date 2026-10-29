"""A release's packages carry its tag's version, and every build ONE's one stamp.

release.yml renamed the game and editor zips and the runtime update to the tag, while
package_godot_windows.ps1 stamped godot/project.godot's config/version into them: the
exes' VERSIONINFO and the runtime package's VERSION (the version the updater tells a
player, ADR 0067 d8). v0.0.19 was tagged on a config/version of 0.0.18 and shipped as
0.0.18. The tag now reaches the script, which stamps it for the exports and puts the
tracked files back after them. The script's builds are the plain runtime and editor zips
the service packs (ADR 0072); its smoke packages carry the same version.

config/version is no source any more (it went stale again: 0.0.21 under v0.0.22). Without
-Version the script takes the latest v* tag, as CMake does, and hands the same version,
commit and build time to every cmake configure as ONE's stamp
(engine/base/version/one_build.h); the STARTUP label reads that stamp, so nothing writes a
build commit into the project. CI's staging version is the stamp's (scripts/ci/one_stamp.sh),
in its builds and in publish.yml alike.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ReleaseVersion(unittest.TestCase):
    def setUp(self):
        workflows = ROOT / ".github" / "workflows"
        self.release = (workflows / "release.yml").read_text(encoding="utf-8")
        self.ci = (workflows / "ci.yml").read_text(encoding="utf-8")
        self.publish = (workflows / "publish.yml").read_text(encoding="utf-8")
        self.script = (ROOT / "scripts" / "package_godot_windows.ps1").read_text(encoding="utf-8")

    def test_the_release_passes_its_tag_to_the_package_script(self):
        step = self.release[self.release.index("- name: Package Godot Windows apps"):]
        step = step[:step.index("- name:", 1)]
        self.assertRegex(step, r"\$env:GITHUB_REF_NAME -replace '\^v', ''")
        self.assertRegex(step, r"package_godot_windows\.ps1 .*-Version \$version")
        self.assertNotIn("-Staging", step)

    def test_the_script_takes_the_version_it_is_given(self):
        params = self.script[self.script.index("param("):self.script.index("\n)\n")]
        self.assertIn('[string]$Version = ""', params)
        self.assertIn('[string]$BuildTime = ""', params)
        self.assertIn("[switch]$Staging", params)
        # Without -Version, the latest v* tag; project.godot's config/version is never read.
        self.assertRegex(
            self.script,
            re.compile(
                r"\nif \(\$Version\) \{.*?\n\} else \{\s*\$tag = & git -C \$ROOT describe --tags --abbrev=0 "
                r"--match 'v\[0-9\]\*'",
                re.S,
            ),
        )
        self.assertNotIn("$projectGodot", self.script)
        for use in ('--version", $Version', "opennova-runtime-windows-v$Version.zip",
                    "opennova-editor-windows-v$Version.zip"):
            self.assertIn(use, self.script)

    def test_every_configure_takes_the_one_stamp(self):
        for key in (
            "OPENNOVA_VERSION=$Version",
            "OPENNOVA_COMMIT=$BuildCommit",
            "OPENNOVA_BUILD_TIME=$BuildTime",
            "OPENNOVA_STAGING=",
        ):
            self.assertIn(key, self.script)
        # Each `cmake -S` command with its backtick-continued lines.
        configures = []
        for match in re.finditer(r"\n\s*cmake -S ", self.script):
            lines = self.script[match.start() + 1 :].split("\n")
            command = [lines[0]]
            while command[-1].rstrip().endswith("`"):
                command.append(lines[len(command)])
            configures.append("\n".join(command))
        self.assertEqual(len(configures), 2, configures)
        for configure in configures:
            self.assertIn("@OneStamp", configure)
        self.assertNotIn("build_commit", self.script)

    def test_the_stamped_files_are_put_back(self):
        stamp = self.script[self.script.index("$presetsOriginal = "):]
        body, restore = stamp.split("finally {", 1)
        self.assertIn('config/version=`"$Version`"', body)
        self.assertIn('application/file_version=`"$WinVersion`"', body)
        self.assertIn("Invoke-GodotExport", body)
        self.assertIn("WriteAllText($presetsPath, $presetsOriginal)", restore)
        self.assertIn("WriteAllText($projectPath, $projectOriginal)", restore)
        # Nothing else writes the tracked files.
        self.assertEqual(len(re.findall(r"Set-Content[^\n]*export_presets|Set-Content -Path \$presetsPath",
                                        self.script)), 0)

    def test_ci_and_publish_share_the_stamp(self):
        # ci.yml takes it once, over a pull request's head (or a staging push's commit) and the
        # run's number.
        self.assertIn('scripts/ci/one_stamp.sh "$HEAD_SHA" $RUN', self.ci)
        self.assertIn("HEAD_SHA: ${{ github.event.pull_request.head.sha || github.sha }}", self.ci)
        self.assertIn(
            "RUN: ${{ (github.event_name == 'pull_request' || github.ref == 'refs/heads/staging') "
            "&& github.run_number || '' }}",
            self.ci,
        )
        package = self.ci[self.ci.index("scripts/package_godot_windows.ps1 -SkipBuild") :]
        package = package[: package.index("- name:")]
        self.assertIn("-Version ${{ needs.changes.outputs.one_version }}", package)
        self.assertIn("-BuildTime ${{ needs.changes.outputs.one_time }}", package)
        # publish.yml's staging version: the same script over the run's head and number.
        self.assertIn(
            'scripts/ci/one_stamp.sh "$head" "$(jq -r .run_number <<<"$run")"', self.publish
        )
        self.assertNotIn("project.godot", self.publish)


if __name__ == "__main__":
    unittest.main()
