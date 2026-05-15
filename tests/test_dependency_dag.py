"""Enforce the host-isolation dependency DAG.

If this test fails, a host-specific module (bpy, pymxs) was imported by a
package that should be host-agnostic.

Runs each check in a subprocess so that polluting ``sys.modules`` in the
parent process does not break later tests that pickle module-defined
classes (e.g. ``opennova_jobs.ImportRequest`` over the multiprocessing
worker pool).
"""
from __future__ import annotations

import subprocess
import sys
import textwrap

import pytest


_FORBIDDEN = {
    "opennova_jobs": {"bpy", "pymxs", "PySide6", "PySide2", "opennova_blender", "opennova_max", "opennova_qt_ui", "apps.onimport"},
    "opennova_qt_ui.backend": {"bpy", "pymxs", "opennova_blender", "opennova_max", "apps.onimport"},
    "opennova_qt_ui.filtering": {"bpy", "pymxs", "opennova_blender", "opennova_max", "PySide6", "PySide2"},
    "opennova_qt_ui.preferences": {"bpy", "pymxs", "opennova_blender", "opennova_max", "PySide6", "PySide2"},
    "pyopennova.scan": {"bpy", "pymxs", "PySide6", "PySide2", "opennova_blender", "opennova_max", "opennova_qt_ui"},
}


def _check_one(target: str, forbidden: set[str]) -> None:
    code = textwrap.dedent(
        f"""
        import importlib, sys

        target = {target!r}
        forbidden = {sorted(forbidden)!r}

        pre = set(sys.modules)
        importlib.import_module(target)
        after = set(sys.modules) - pre

        violations = []
        for mod in after:
            for ban in forbidden:
                if mod == ban or mod.startswith(ban + "."):
                    violations.append((target, ban, mod))
        if violations:
            for target, ban, mod in violations:
                print(f"VIOLATION: {{target}} pulled in forbidden {{ban}} via {{mod}}")
            sys.exit(1)
        """
    )
    result = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        pytest.fail(
            f"DAG check failed for {target}:\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )


@pytest.mark.parametrize("target,forbidden", sorted(_FORBIDDEN.items()))
def test_dag_target(target: str, forbidden: set[str]) -> None:
    _check_one(target, forbidden)
