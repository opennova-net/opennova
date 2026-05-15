"""Validation script for opennova_max: runs under 3dsmaxbatch.exe.

Pinpoints any Python 3.7-vs-PEP-585 incompatibility in the OpenNova Max
plugin by exercising the import path with traceback.format_exc() capture.

Usage (from this worktree):
  & "C:\\Program Files\\Autodesk\\3ds Max 2022\\3dsmaxbatch.exe" \
      scripts\\max_validation_script.py -v 3

Emits milestones (parsed by tests/test_max_local_batch.py) and writes the
full traceback (if any) to build-max-trace/trace.txt for downstream
inspection.
"""
from __future__ import annotations

import os
import sys
import traceback
from pathlib import Path


WORKTREE = Path(__file__).resolve().parents[1]
TRACE_DIR = WORKTREE / "build-max-trace"
TRACE_PATH = TRACE_DIR / "trace.txt"
SMOKE_FIXTURE = WORKTREE / "fixtures" / "threedi" / "3di3" / "Shed.3di"


def _emit(line: str) -> None:
    print(line, flush=True)


def _write_trace(label: str, exc: BaseException) -> None:
    TRACE_DIR.mkdir(parents=True, exist_ok=True)
    with TRACE_PATH.open("a", encoding="utf-8") as f:
        f.write(f"\n\n=== {label} ===\n")
        f.write(f"Exception: {type(exc).__name__}: {exc}\n")
        f.write(traceback.format_exc())


def _reset_trace() -> None:
    TRACE_DIR.mkdir(parents=True, exist_ok=True)
    if TRACE_PATH.exists():
        TRACE_PATH.unlink()


def _prepend_path() -> None:
    sys.path.insert(0, str(WORKTREE))


def _smoke_import_all_modules() -> bool:
    """Import every module in opennova_max (and direct deps).

    Catches module-load-time PEP 585 issues before we attempt a real
    import workflow.
    """
    targets = [
        "pyopennova",
        "pyopennova.threedi_ffi",
        "pyopennova.asset_resolver",
        "pyopennova.scan",
        "pyopennova.resource_plan",
        "pyopennova.mesh_build",
        "pyopennova.coords",
        "pyopennova.polyhedron",
        "pyopennova.model_access",
        "pyopennova.scene_naming",
        "pyopennova.mesh_utils",
        "pyopennova.mesh_primitives",
        "opennova_jobs",
        "opennova_qt_ui",
        "opennova_qt_ui.backend",
        "opennova_max",
        "opennova_max.version",
        "opennova_max.backend",
        "opennova_max.import_runner",
        "opennova_max.scene_builder",
        "opennova_max.mesh",
        "opennova_max.materials",
        "opennova_max.animation",
        "opennova_max.output_writers",
        "opennova_max.flat_mesh_bridge",
        "opennova_max.ase_export",
        "opennova_max.ui",
        "opennova_max.qt_ui",
    ]
    any_failure = False
    for mod in targets:
        try:
            __import__(mod)
            _emit(f"OPENNOVA_MAX_MODULE_OK {mod}")
        except BaseException as exc:
            any_failure = True
            _emit(f"OPENNOVA_MAX_MODULE_FAIL {mod}: {type(exc).__name__}: {exc}")
            _write_trace(f"import {mod}", exc)
    return not any_failure


def _smoke_loose_import() -> bool:
    """Run a loose 3DI import to exercise the scene builder."""
    if not SMOKE_FIXTURE.exists():
        _emit(f"OPENNOVA_MAX_FIXTURE_MISSING {SMOKE_FIXTURE}")
        return False

    out_dir = TRACE_DIR / "loose-out"
    try:
        from opennova_max.import_runner import run_loose_import

        ok = run_loose_import(
            str(SMOKE_FIXTURE),
            str(out_dir),
            output_stem="Shed",
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            write_ase=True,
            write_3dp=True,
            write_max=False,
            copy_textures=False,
            reset_scene=True,
        )
        _emit(f"OPENNOVA_MAX_LOOSE_RESULT {bool(ok)}")
        return bool(ok)
    except BaseException as exc:
        _emit(f"OPENNOVA_MAX_LOOSE_FAIL: {type(exc).__name__}: {exc}")
        _write_trace("run_loose_import(Shed.3di)", exc)
        return False


def _smoke_definition_import() -> bool:
    """Run a definition (item-type) import if game data is available.

    Driven by env vars:
      OPENNOVA_MAX_DEF_BASE_DIR   asset base dir (e.g. ~/Desktop/JO_CLIENT)
      OPENNOVA_MAX_DEF_ITEM_NAME  asset name (e.g. 'Airport Terminal building')
      OPENNOVA_MAX_DEF_ITEM_TYPE  asset type (e.g. 'item')

    Skips silently when not set.
    """
    base_dir = os.environ.get("OPENNOVA_MAX_DEF_BASE_DIR")
    item_name = os.environ.get("OPENNOVA_MAX_DEF_ITEM_NAME")
    item_type = os.environ.get("OPENNOVA_MAX_DEF_ITEM_TYPE", "item")
    if not (base_dir and item_name):
        _emit("OPENNOVA_MAX_DEF_SKIPPED (no OPENNOVA_MAX_DEF_BASE_DIR/ITEM_NAME)")
        return True

    out_dir = TRACE_DIR / "def-out"
    try:
        from opennova_max.import_runner import run_import

        ok = run_import(
            base_dir,
            item_name,
            item_type,
            str(out_dir),
            write_ase=True,
            write_3dp=True,
            write_max=False,
            copy_textures=False,
            reset_scene=True,
        )
        _emit(f"OPENNOVA_MAX_DEF_RESULT {bool(ok)} ({item_type} {item_name})")
        return bool(ok)
    except BaseException as exc:
        _emit(f"OPENNOVA_MAX_DEF_FAIL ({item_type} {item_name}): "
              f"{type(exc).__name__}: {exc}")
        _write_trace(f"run_import({item_type} {item_name})", exc)
        return False


def main() -> int:
    _emit("OPENNOVA_MAX_VALIDATION_START")
    _reset_trace()
    _prepend_path()
    _emit(f"OPENNOVA_MAX_PYTHON_VERSION {sys.version.split()[0]}")
    _emit(f"OPENNOVA_MAX_SYS_PATH_0 {sys.path[0]}")

    all_ok = True
    if not _smoke_import_all_modules():
        all_ok = False
    if not _smoke_loose_import():
        all_ok = False
    if not _smoke_definition_import():
        all_ok = False

    _emit(f"OPENNOVA_MAX_VALIDATION_RESULT {all_ok}")
    if TRACE_PATH.exists() and TRACE_PATH.stat().st_size > 0:
        _emit(f"OPENNOVA_MAX_TRACE_PATH {TRACE_PATH}")
    return 0 if all_ok else 1


main()
