"""A release's packages carry its tag's version.

release.yml renamed the game and editor zips and the runtime update to the tag, while
package_godot_windows.ps1 stamped godot/project.godot's config/version into them: the
exes' VERSIONINFO and the runtime package's VERSION (the version the updater tells a
player, ADR 0067 d8). v0.0.19 was tagged on a config/version of 0.0.18 and shipped as
0.0.18. The tag now reaches the script, which stamps it for the exports and puts the
tracked files back after them. The script's builds are the plain runtime and editor zips
the service packs (ADR 0072); its smoke packages carry the same version.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ReleaseVersion(unittest.TestCase):
    def setUp(self):
        self.release = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.script = (ROOT / "scripts" / "package_godot_windows.ps1").read_text(encoding="utf-8")

    def test_the_release_passes_its_tag_to_the_package_script(self):
        step = self.release[self.release.index("- name: Package Godot Windows apps"):]
        step = step[:step.index("- name:", 1)]
        self.assertRegex(step, r"\$env:GITHUB_REF_NAME -replace '\^v', ''")
        self.assertRegex(step, r"package_godot_windows\.ps1 .*-Version \$version")

    def test_the_script_takes_the_version_it_is_given(self):
        params = self.script[self.script.index("param("):self.script.index("\n)\n")]
        self.assertIn('[string]$Version = ""', params)
        # project.godot is read only when no -Version is given.
        self.assertRegex(self.script, re.compile(r"\nif \(\$Version\) \{.*?\n\} else \{\s*\$projectGodot = ", re.S))
        for use in ('--version", $Version', "opennova-runtime-windows-v$Version.zip",
                    "opennova-editor-windows-v$Version.zip"):
            self.assertIn(use, self.script)

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


if __name__ == "__main__":
    unittest.main()
