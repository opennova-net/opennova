"""
ctypes bindings for the DEF file C API (libopennova.so / opennova.dll).

Mirrors structs from libs/def/include/def/def.h.
Only wraps weapons and items parsing (what definitions.py actually consumes).
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Struct definitions matching def.h
# ---------------------------------------------------------------------------

class DefWeaponAction(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("anim", ctypes.c_char * 128),
        ("function", ctypes.c_char * 128),
        ("delaystart", ctypes.c_int),
        ("delayend", ctypes.c_int),
        ("soundset", ctypes.c_char * 128),
        ("soundsetend", ctypes.c_char * 128),
        ("particle", ctypes.c_char * 128),
        ("particleuserpoint", ctypes.c_char * 128),
        ("raw_lines", ctypes.c_void_p),
        ("raw_lines_count", ctypes.c_size_t),
    ]


class DefWeaponDef(ctypes.Structure):
    _fields_ = [
        ("weapon_name", ctypes.c_char * 64),
        ("category", ctypes.c_int),
        ("rank", ctypes.c_int),
        ("clipsize", ctypes.c_int),
        ("startrounds", ctypes.c_int),
        ("statid", ctypes.c_int),
        ("maxclips", ctypes.c_int),
        ("ammobucket", ctypes.c_int),
        ("ammo_class", ctypes.c_char * 64),
        ("ammo_class_count", ctypes.c_int),
        ("charfilter", (ctypes.c_char * 16) * 8),
        ("charfilter_count", ctypes.c_size_t),
        ("teamfilter", (ctypes.c_char * 16) * 4),
        ("teamfilter_count", ctypes.c_size_t),
        ("loadout_selectable", ctypes.c_int),
        ("loadout_subclasses", ctypes.c_int),
        ("weapon_class", ctypes.c_char * 32),
        ("round_type", ctypes.c_char * 64),
        ("animadm", ctypes.c_char * 128),
        ("launch_user_point", ctypes.c_char * 64),
        ("gfx1", ctypes.c_char * 128),
        ("gfx1a", ctypes.c_char * 128),
        ("gfx1b", ctypes.c_char * 128),
        ("gfx3", ctypes.c_char * 128),
        ("crosshair", ctypes.c_char * 128),
        ("hudicon", ctypes.c_char * 128),
        ("hudclipgfx_texture", ctypes.c_char * 128),
        ("hudclipgfx_offset", ctypes.c_int * 2),
        ("hudrndgfx_texture", ctypes.c_char * 128),
        ("hudrndgfx_offset", ctypes.c_int * 2),
        ("hudrndgfx_layout", ctypes.c_int * 3),
        ("actions", ctypes.POINTER(DefWeaponAction)),
        ("actions_count", ctypes.c_size_t),
        ("flags", ctypes.c_int),
        ("error", ctypes.c_float * 6),
        ("pos", ctypes.c_float * 6),
        ("tpos", ctypes.c_float * 6),
        ("sights", ctypes.c_void_p),  # DefSightEntry*, not needed by blender
        ("sights_count", ctypes.c_size_t),
        ("raw_lines", ctypes.c_void_p),
        ("raw_lines_count", ctypes.c_size_t),
        # PLAYER_INFO loadout fields (appended; mirror libs/def/include/def/def.h).
        # loadout_selectable/loadout_subclasses/maxclips live above with the armory keys.
        ("loadout_menu_textid", ctypes.c_char * 64),
        ("loadout_menu_ttdesc", ctypes.c_char * 128),
        ("loadout_menu_icon", ctypes.c_char * 64),
        ("weapon_class_slot", ctypes.c_int),
        ("teamfilter_mask", ctypes.c_int),
        ("charfilter_mask", ctypes.c_int),
        ("weaponweight", ctypes.c_float),
        ("clipweight", ctypes.c_float),
        # First-person render fov, horizontal degrees (default 80.0; mirror def.h).
        ("renderfov", ctypes.c_float),
        # ADS zoom magnification ('scope_max_mag'; 0 = key absent; mirror def.h).
        ("scope_max_mag", ctypes.c_float),
    ]


class DefWeaponsFile(ctypes.Structure):
    _fields_ = [
        ("ammo_class_lines", ctypes.c_void_p),
        ("ammo_class_lines_count", ctypes.c_size_t),
        ("entries", ctypes.POINTER(DefWeaponDef)),
        ("count", ctypes.c_size_t),
    ]


class DefItemDef(ctypes.Structure):
    _fields_ = [
        ("display_name", ctypes.c_char * 128),
        ("id", ctypes.c_int),
        ("sid", ctypes.c_char * 64),
        ("type", ctypes.c_int),
        ("graphic", ctypes.c_char * 128),
        ("anim_def", ctypes.c_char * 128),
        ("husk", ctypes.c_char * 128),
        ("hp", ctypes.c_int),
        ("sound_profile", ctypes.c_char * 128),
        ("soundloops", (ctypes.c_char * 128) * 7),
        ("nightshot", ctypes.c_char * 128),
        ("dawnshot", ctypes.c_char * 128),
        ("duskshot", ctypes.c_char * 128),
        ("dayshot", ctypes.c_char * 128),
        ("raw_lines", ctypes.c_void_p),
        ("raw_lines_count", ctypes.c_size_t),
    ]


class DefItemsFile(ctypes.Structure):
    _fields_ = [
        ("entries", ctypes.POINTER(DefItemDef)),
        ("count", ctypes.c_size_t),
    ]


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

_bound = False


def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()
    lib.def_parse_weapons.restype = ctypes.c_int
    lib.def_parse_weapons.argtypes = [ctypes.c_char_p, ctypes.POINTER(DefWeaponsFile)]
    lib.def_free_weapons.restype = None
    lib.def_free_weapons.argtypes = [ctypes.POINTER(DefWeaponsFile)]
    lib.def_parse_items.restype = ctypes.c_int
    lib.def_parse_items.argtypes = [ctypes.c_char_p, ctypes.POINTER(DefItemsFile)]
    lib.def_free_items.restype = None
    lib.def_free_items.argtypes = [ctypes.POINTER(DefItemsFile)]
    _bound = True


def parse_weapons_def(path: str) -> DefWeaponsFile:
    """Parse a weapon.def file. Caller must call free_weapons_def() when done."""
    _bind()
    lib = load_lib()
    wf = DefWeaponsFile()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.def_parse_weapons(path, ctypes.byref(wf))
    if rc != 0:
        raise RuntimeError(f"def_parse_weapons failed for {path!r}")
    return wf


def free_weapons_def(wf: DefWeaponsFile):
    """Free all C-side allocations inside a DefWeaponsFile."""
    _bind()
    lib = load_lib()
    lib.def_free_weapons(ctypes.byref(wf))


def parse_items_def(path: str) -> DefItemsFile:
    """Parse an items.def file. Caller must call free_items_def() when done."""
    _bind()
    lib = load_lib()
    ifl = DefItemsFile()
    if isinstance(path, str):
        path = path.encode("utf-8")
    rc = lib.def_parse_items(path, ctypes.byref(ifl))
    if rc != 0:
        raise RuntimeError(f"def_parse_items failed for {path!r}")
    return ifl


def free_items_def(ifl: DefItemsFile):
    """Free all C-side allocations inside a DefItemsFile."""
    _bind()
    lib = load_lib()
    lib.def_free_items(ctypes.byref(ifl))
