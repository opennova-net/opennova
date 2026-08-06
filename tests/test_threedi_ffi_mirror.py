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


def test_all_mirror_structs_are_packed():
    # threedi_3di3.h wraps every struct in #pragma pack(push, 1); a mirror
    # without _pack_ = 1 silently pads and corrupts every field after the
    # first misalignment.
    for name, cls in sorted(_structs(py_threedi).items()):
        assert getattr(cls, "_pack_", None) == 1, f"{name}: _pack_ != 1"


def test_packed_struct_sizes_pin_native_abi():
    # Pinned against the C header's #pragma pack(1) layouts (the C side
    # static_asserts the same numbers); drift on either side fails here.
    assert ctypes.sizeof(py_threedi.ThreediTransform) == 8
    assert ctypes.sizeof(py_threedi.ThreediPartAnimation) == 0x44
    assert ctypes.sizeof(py_threedi.ThreediLight) == 116
    assert ctypes.sizeof(py_threedi.ThreediBoundingVolume) == 36
    assert ctypes.sizeof(py_threedi.ThreediCollisionTranslation) == 12
    assert ctypes.sizeof(py_threedi.ThreediCollisionObject) == 88
    assert ctypes.sizeof(py_threedi.ThreediCollisionFace) == 44
    assert ctypes.sizeof(py_threedi.ThreediCollisionNormal) == 14


def test_collision_face_runtime_cfac_layout():
    # The 44-B runtime CFAC record the raycast code walks.
    face = py_threedi.ThreediCollisionFace
    assert ctypes.sizeof(face) == 44
    assert [(name, getattr(face, name).offset) for name in (
        "normal_index", "plane_dist_fp16", "material_flags", "poly_type",
    )] == [
        ("normal_index", 6),
        ("plane_dist_fp16", 8),
        ("material_flags", 36),
        ("poly_type", 40),
    ]


def test_collision_object_exact_cobj_sphere_layout():
    obj = py_threedi.ThreediCollisionObject
    assert ctypes.sizeof(obj) == 88
    assert [(name, getattr(obj, name).offset) for name in (
        "offset", "med", "radius",
    )] == [
        ("offset", 36),
        ("med", 72),
        ("radius", 84),
    ]
