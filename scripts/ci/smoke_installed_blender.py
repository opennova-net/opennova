"""Run with Blender against the freshly installed extension, without fixing its files."""
import importlib
import os
from pathlib import Path
import shutil
import stat
import tempfile

import addon_utils
import bpy

NAME = "bl_ext.user_default.opennova_3di"
addon_utils.enable(NAME, default_set=True)
addon = importlib.import_module(NAME)
cli = Path(addon.o3dtext.bundled_cli_path())
addon.o3dtext.run_cli(None, ["catalog"], addon.o3dtext.ExportError)
assert os.access(cli, os.X_OK), "Installed bundled CLI must run without a manual chmod"

if os.name == "posix":
    # Repeat after stripping the bit to cover repair even if Blender starts
    # preserving it in a later installer. Preserve unrelated permission bits.
    original = stat.S_IMODE(cli.stat().st_mode)
    stripped = original & ~0o111
    cli.chmod(stripped)
    try:
        addon.o3dtext.run_cli(None, ["catalog"], addon.o3dtext.ExportError)
        assert stat.S_IMODE(cli.stat().st_mode) == stripped | stat.S_IXUSR
    finally:
        cli.chmod(original)
    with tempfile.TemporaryDirectory(prefix="opennova-custom-cli-") as scratch:
        custom = Path(scratch) / "opennova-3di"
        shutil.copyfile(cli, custom)
        custom.chmod(0o644)
        preference = bpy.context.preferences.addons[NAME].preferences
        preference.cli_path = str(custom)
        try:
            try:
                addon.o3dtext.run_cli(None, ["catalog"], addon.o3dtext.ExportError)
            except addon.o3dtext.ExportError:
                pass
            else:
                raise AssertionError("A custom nonexecutable CLI must fail")
            assert stat.S_IMODE(custom.stat().st_mode) == 0o644, "Custom CLI permissions changed"
        finally:
            preference.cli_path = ""

print("Installed Blender CLI smoke passed")
