"""The add-on a tests/blender/*_test.py exercises, loaded one of two ways:

    blender -b --factory-startup --python-exit-code 1 --python tests/blender/<x>_test.py -- <opennova-3di.exe>

loads the source tree's add-on (tools/blender/opennova_3di) and runs that
opennova-3di;

    blender -b --factory-startup --python-exit-code 1 --python tests/blender/<x>_test.py -- --installed

enables the extension installed into this Blender (`blender --command
extension install-file` puts it in the user_default repository) and runs the
opennova-3di bundled in it, so the packaged zip is what is tested.

A test does `sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))`,
`import addon_harness`, then `addon, cli = addon_harness.load()`: the add-on
package (its modules as attributes: addon.export, addon.materials, ...)
registered, the scene emptied, and the path of the CLI it runs; and writes
its files under `addon_harness.scratch(prefix)`. Its name keeps it out of the
*_test.py set that runs as tests.
"""
import atexit
import importlib
import os
import shutil
import sys
import tempfile

import bpy


def scratch(prefix):
    """A fresh temporary directory for a test's files, removed when Blender
    exits (whether the test passed or not)."""
    path = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, path, ignore_errors=True)
    return path

INSTALLED = "bl_ext.user_default.opennova_3di"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def arguments():
    """The script's own arguments: what follows Blender's `--`."""
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def load():
    """(the add-on package, the opennova-3di it runs), on an empty scene."""
    import addon_utils
    args = arguments()
    if args[:1] == ["--installed"]:
        addon_utils.enable(INSTALLED, default_set=True)
        addon = importlib.import_module(INSTALLED)
        cli = addon.o3dtext.bundled_cli_path()
    elif args:
        # Enabled as Blender enables an add-on, so its preferences exist: they
        # name the opennova-3di it runs.
        sys.path.insert(0, os.path.join(ROOT, "tools", "blender"))
        addon_utils.enable("opennova_3di", default_set=True)
        addon = importlib.import_module("opennova_3di")
        cli = os.path.abspath(args[0])
        bpy.context.preferences.addons["opennova_3di"].preferences.cli_path = cli
    else:
        raise SystemExit("usage: blender -b --factory-startup --python-exit-code 1 --python <test> -- "
                         "<opennova-3di.exe> | --installed")
    if not os.path.isfile(cli):
        raise SystemExit(f"opennova-3di not found at {cli}")
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    return addon, cli
