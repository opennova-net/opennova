from __future__ import annotations

import subprocess
import sys
import textwrap

import pytest


_FORBIDDEN = {
    "opennova_jobs": {"bpy", "pymxs", "PySide6", "PySide2", "opennova_blender", "opennova_qt_ui"},
    "opennova_qt_ui.backend": {"bpy", "pymxs", "opennova_blender", "apps.importer"},
    "opennova_qt_ui.collisions": {"bpy", "pymxs", "opennova_blender", "PySide6", "PySide2"},
    "opennova_qt_ui.filtering": {"bpy", "pymxs", "opennova_blender", "PySide6", "PySide2"},
    "opennova_qt_ui.preferences": {"bpy", "pymxs", "opennova_blender", "PySide6", "PySide2"},
}


def _check_one(target: str, forbidden: set[str]) -> None:
    code = textwrap.dedent(
        f"""
        import importlib, sys

        pre = set(sys.modules)
        importlib.import_module({target!r})
        after = set(sys.modules) - pre
        violations = []
        for mod in after:
            for ban in {sorted(forbidden)!r}:
                if mod == ban or mod.startswith(ban + "."):
                    violations.append((ban, mod))
        if violations:
            for ban, mod in violations:
                print(f"VIOLATION: {target} pulled in {{ban}} via {{mod}}")
            sys.exit(1)
        """
    )
    result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    if result.returncode != 0:
        pytest.fail(
            f"DAG check failed for {target}:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


@pytest.mark.parametrize("target,forbidden", sorted(_FORBIDDEN.items()))
def test_dag_target(target: str, forbidden: set[str]) -> None:
    _check_one(target, forbidden)
