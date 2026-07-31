"""The def.h ctypes mirrors must agree with each other — and with the native stride.

pyopennova/def_ffi.py and blender/opennova/def_ffi.py each mirror
libs/def/include/def/def.h by hand; a field missed in one changes that mirror's
ARRAY STRIDE, so every `entries[i]` after index zero reads shifted bytes —
silently (names and floats just come out wrong; scope_max_mag went missing from
the Blender mirror exactly this way). Two nets:

- layout parity: every struct the Blender mirror declares must match the
  pyopennova mirror field-for-field (name, offset, size) and in total size;
- a multi-entry parse of the tracked fixtures/def/weapon.def through EACH
  mirror's stride (skipped when the native library is absent).
"""
from __future__ import annotations

import ctypes
import importlib.util
import sys
import types
from pathlib import Path

import pytest

from pyopennova import def_ffi as py_def

REPO = Path(__file__).resolve().parent.parent
WEAPON_DEF = REPO / "fixtures" / "def" / "weapon.def"
ITEMS_DEF = REPO / "fixtures" / "def" / "items.def"
# The fixture's first weapon blocks, in file order (the stride bug reads entry 0
# fine and garbles every later one, so assert well past index zero).
FIXTURE_HEAD = ["WPN_KNIFE", "WPN_KNIFE2", "WPN_colt45", "WPN_M9Beretta"]
# items.def stride canary: entry 0 always reads fine; a stale DefItemDef mirror
# garbles every later display name (this exact desync shipped once — the
# ai_function..unit_type block landed C-side without the mirrors).
ITEMS_HEAD = ["Drivable Dune Buggy", "ATV Quad", "Marker Alpha", "Guard Tower"]


def _load_blender_def_ffi():
    """Import blender/opennova/def_ffi.py standalone: give it a package shell and
    a stub `_native` so the import needs neither bpy nor the built library."""
    pkg_name = "_blender_opennova_mirror_test"
    pkg = types.ModuleType(pkg_name)
    pkg.__path__ = [str(REPO / "blender" / "opennova")]
    sys.modules[pkg_name] = pkg
    stub = types.ModuleType(pkg_name + "._native")

    def _layout_only_load_lib():
        raise RuntimeError("layout-only import: the native library is not bound here")

    stub.load_lib = _layout_only_load_lib
    sys.modules[pkg_name + "._native"] = stub
    spec = importlib.util.spec_from_file_location(
        pkg_name + ".def_ffi", REPO / "blender" / "opennova" / "def_ffi.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[pkg_name + ".def_ffi"] = mod
    spec.loader.exec_module(mod)
    return mod


def _structs(mod) -> dict:
    return {
        name: obj
        for name, obj in vars(mod).items()
        if isinstance(obj, type) and issubclass(obj, ctypes.Structure)
    }


def _layout(struct_cls) -> list:
    out = []
    for name, _ctype in struct_cls._fields_:
        descriptor = getattr(struct_cls, name)
        out.append((name, descriptor.offset, descriptor.size))
    return out


def test_blender_mirror_layouts_match_pyopennova():
    blender = _load_blender_def_ffi()
    blender_structs = _structs(blender)
    assert blender_structs, "the Blender mirror declares ctypes structs"
    for name, blender_cls in sorted(blender_structs.items()):
        py_cls = getattr(py_def, name, None)
        assert py_cls is not None, f"{name}: in the Blender mirror but not pyopennova's"
        assert _layout(py_cls) == _layout(blender_cls), (
            f"{name}: field layout diverged between the mirrors")
        assert ctypes.sizeof(py_cls) == ctypes.sizeof(blender_cls), (
            f"{name}: sizeof diverged — the array stride is broken in one mirror")


def test_weapon_exact_carriers_are_append_only_in_both_mirrors():
    """The native DefWeaponDef appends these fields to preserve all prior offsets.

    Pinning the tail independently of the cross-mirror comparison prevents both
    mirrors from accidentally agreeing on the same stale, short array stride.
    """
    expected_tail = [
        "error_fp16",
        "error_hip_theta_fp16",
        "error_up_theta_fp16",
        "weaponweight_fp16",
        "clipweight_fp16",
    ]
    blender = _load_blender_def_ffi()
    for weapon_cls in (py_def.DefWeaponDef, blender.DefWeaponDef):
        names = [name for name, _ctype in weapon_cls._fields_]
        assert names[-len(expected_tail):] == expected_tail
        assert weapon_cls.error_fp16.size == ctypes.sizeof(ctypes.c_int * 6)
        fields_end = weapon_cls.clipweight_fp16.offset + ctypes.sizeof(ctypes.c_int)
        assert fields_end <= ctypes.sizeof(weapon_cls)
        assert ctypes.sizeof(weapon_cls) - fields_end < ctypes.alignment(weapon_cls)


def _skip_without_native():
    try:
        py_def._bind()
    except Exception as exc:  # pragma: no cover - environment-dependent
        pytest.skip(f"native opennova library unavailable: {exc}")


def test_pyopennova_stride_reads_every_entry():
    _skip_without_native()
    wf = py_def.parse_weapons_def(str(WEAPON_DEF))
    try:
        names = [wf.entries[i].weapon_name.decode() for i in range(len(FIXTURE_HEAD))]
    finally:
        py_def.free_weapons_def(wf)
    assert names == FIXTURE_HEAD


def test_pyopennova_items_stride_reads_every_entry():
    _skip_without_native()
    ifl = py_def.parse_items_def(str(ITEMS_DEF))
    try:
        names = [ifl.entries[i].display_name.decode() for i in range(len(ITEMS_HEAD))]
        # The particle keys ride the same stride — the Dune Buggy's authored
        # slot-A row is the deep-field canary.
        fx = ifl.entries[0].particlefx
        assert fx.effect.decode() == "Effect_whiteExhaust"
        assert fx.userpoint.decode() == "FX00"
    finally:
        py_def.free_items_def(ifl)
    assert names == ITEMS_HEAD


def test_pyopennova_emplacement_attachment_nested_layout(tmp_path):
    _skip_without_native()
    path = tmp_path / "attachment_items.def"
    path.write_text(
        "begin AttachmentCarrier\n"
        "  id 710100\n"
        "  addeweapG abcdefghijklmnopq 710102 70 10 100 90\n"
        "end\n",
        encoding="ascii",
    )
    ifl = py_def.parse_items_def(str(path))
    try:
        assert ifl.count == 1
        carrier = ifl.entries[0]
        assert carrier.emplacement_attachments_count == 1
        row = carrier.emplacement_attachments[0]
        assert row.userpoint.decode() == "abcdefghijklmno"
        assert row.item_id == 710102
        assert row.down_angle == 70 * 11930464
        assert row.up_angle == -10 * 11930464
        assert row.right_angle == 100 * 11930464
        assert row.left_angle == -90 * 11930464
        assert row.angle_count == 4
        assert row.kind == 1
        assert carrier.emplacement_g_slot == 1
        assert carrier.emplacement_c_slot == 0
    finally:
        py_def.free_items_def(ifl)


def test_blender_stride_reads_every_entry():
    _skip_without_native()
    blender = _load_blender_def_ffi()
    lib = py_def.load_lib()
    # Bind the SAME native entry points against the Blender mirror's structs;
    # a short struct here reads every entry after index zero 8 bytes early.
    lib.def_parse_weapons.restype = ctypes.c_int
    lib.def_parse_weapons.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(blender.DefWeaponsFile)]
    lib.def_free_weapons.restype = None
    lib.def_free_weapons.argtypes = [ctypes.POINTER(blender.DefWeaponsFile)]
    wf = blender.DefWeaponsFile()
    rc = lib.def_parse_weapons(str(WEAPON_DEF).encode(), ctypes.byref(wf))
    assert rc == 0, "def_parse_weapons failed on the tracked fixture"
    try:
        assert wf.count >= len(FIXTURE_HEAD)
        names = [wf.entries[i].weapon_name.decode() for i in range(len(FIXTURE_HEAD))]
    finally:
        lib.def_free_weapons(ctypes.byref(wf))
        # Leave the shared handle bound the way pyopennova's _bind() expects.
        lib.def_parse_weapons.argtypes = [
            ctypes.c_char_p, ctypes.POINTER(py_def.DefWeaponsFile)]
        lib.def_free_weapons.argtypes = [ctypes.POINTER(py_def.DefWeaponsFile)]
    assert names == FIXTURE_HEAD
