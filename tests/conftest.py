"""Pytest harness setup for Blender-adjacent modules.

Tests frequently import ``blender.opennova`` FFI modules, but importing the
top-level Blender add-on package executes ``blender/__init__.py`` and imports
real ``bpy``. The standalone bpy package corrupts later importer worker
subprocess imports when loaded in the parent pytest process, so keep
``blender`` as a namespace-style stub for normal tests. Real add-on/operator
coverage runs in child Python processes.
"""
from __future__ import annotations

from apps.importer.import_runner import _setup_blender_package


def pytest_configure() -> None:
    _setup_blender_package()
