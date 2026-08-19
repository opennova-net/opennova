"""Run in-process ``bpy`` test modules after everything else.

The standalone ``bpy`` package corrupts SUBPROCESS imports once it has been
loaded in the parent pytest process — importer workers spawned afterwards die
with ``No module named '_bpy'`` — so every module that imports ``bpy``
in-process declares ``USES_INPROCESS_BPY = True`` and is pushed to the end of
the collection, behind the dispatcher/worker suites. (This ordering used to
ride on alphabetical file names: ``test_scene_builder_*`` happened to sort
after ``test_importer_*``.)
"""
from __future__ import annotations


def pytest_collection_modifyitems(session, config, items) -> None:
    del session, config

    def uses_inprocess_bpy(item) -> bool:
        module = getattr(item, "module", None)
        return bool(getattr(module, "USES_INPROCESS_BPY", False))

    items.sort(key=uses_inprocess_bpy)
