"""The two hand-written 3DI ctypes mirrors must keep identical ABI layouts."""
from __future__ import annotations

import ctypes
import importlib.util
import sys
import types
from pathlib import Path

from pyopennova import threedi_ffi as py_threedi


REPO = Path(__file__).resolve().parent.parent


def _load_blender_threedi_ffi():
    pkg_name = "_blender_opennova_threedi_mirror_test"
    pkg = types.ModuleType(pkg_name)
    pkg.__path__ = [str(REPO / "blender" / "opennova")]
    sys.modules[pkg_name] = pkg
    stub = types.ModuleType(pkg_name + "._native")
    stub.load_lib = lambda: (_ for _ in ()).throw(
        RuntimeError("layout-only import: native library is not bound"))
    sys.modules[pkg_name + "._native"] = stub
    spec = importlib.util.spec_from_file_location(
        pkg_name + ".threedi_ffi",
        REPO / "blender" / "opennova" / "threedi_ffi.py",
    )
    mod = importlib.util.module_from_spec(spec)
    sys.modules[pkg_name + ".threedi_ffi"] = mod
    spec.loader.exec_module(mod)
    return mod


def _structs(mod) -> dict[str, type[ctypes.Structure]]:
    return {
        name: obj
        for name, obj in vars(mod).items()
        if isinstance(obj, type) and issubclass(obj, ctypes.Structure)
    }


def _layout(struct_cls) -> list[tuple[str, int, int]]:
    return [
        (name, getattr(struct_cls, name).offset, getattr(struct_cls, name).size)
        for name, _ctype in struct_cls._fields_
    ]


def test_blender_mirror_layouts_match_pyopennova():
    blender = _load_blender_threedi_ffi()
    for name, blender_cls in sorted(_structs(blender).items()):
        py_cls = getattr(py_threedi, name, None)
        assert py_cls is not None, f"{name}: missing from pyopennova mirror"
        assert _layout(py_cls) == _layout(blender_cls), f"{name}: field layout diverged"
        assert ctypes.sizeof(py_cls) == ctypes.sizeof(blender_cls), (
            f"{name}: ctypes array stride diverged"
        )


def test_collision_face_round_raycast_layout():
    face = py_threedi.ThreediIRCollisionFace
    assert ctypes.sizeof(face) == 52
    assert [(name, getattr(face, name).offset) for name in (
        "material_flags", "poly_type", "normal", "dominate_axis",
        "plane_dist_fp16", "min_fp16", "max_fp16",
    )] == [
        ("material_flags", 8),
        ("poly_type", 12),
        ("normal", 14),
        ("dominate_axis", 20),
        ("plane_dist_fp16", 24),
        ("min_fp16", 28),
        ("max_fp16", 40),
    ]


def test_collision_object_exact_cobj_sphere_layout():
    obj = py_threedi.ThreediIRCollisionObject
    assert ctypes.sizeof(obj) == 72
    assert [(name, getattr(obj, name).offset) for name in (
        "center_fp16", "radius_fp16",
    )] == [
        ("center_fp16", 56),
        ("radius_fp16", 68),
    ]
