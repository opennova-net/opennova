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
        # 3P body-channel kinds ('special_hold' 1..8 hold-pose ladder,
        # 'attack_anim' 1 knife / 2 grenade; 0 = key absent; mirror def.h).
        ("special_hold", ctypes.c_int),
        ("attack_anim", ctypes.c_int),
        # The second FLAGS dword (NoSelect/.../Inset; appended; mirror def.h).
        ("flags2", ctypes.c_int),
        # Run-gait class ('run_anim'; 0 = key absent; mirror def.h).
        ("run_anim", ctypes.c_int),
        # Attach-label text key ('attachtextid'; empty = key absent; mirror def.h).
        ("attach_text_id", ctypes.c_char * 32),
        # Per-class startrounds overrides ('classrounds'; raw value-table indices
        # medic=1 sniper=2 gunner=3 rifleman=5 engineer=6; mirror def.h).
        ("classrounds", ctypes.c_int * 7),
        # Post-recoil auto-switch category ('switchcategory'; mirror def.h).
        ("switchcategory", ctypes.c_int),
        ("has_switchcategory", ctypes.c_int),
        # The weapon heat model ('heat_values' / 'heat_effect'; mirror def.h).
        # Both heat_values fields are 16.16 and PRE-DIVIDED by the parser (by 100
        # and by 6200); the runtime divides them against each other.
        ("heat_per_shot", ctypes.c_int),
        ("heat_decay_per_tick", ctypes.c_int),
        ("heat_glow_threshold", ctypes.c_int),
        ("heat_effect", ctypes.c_char * 64),
        # Exact 16.16 spread/theta/weight carriers (append-only; mirror def.h).
        ("error_fp16", ctypes.c_int * 6),
        ("error_hip_theta_fp16", ctypes.c_int),
        ("error_up_theta_fp16", ctypes.c_int),
        ("weaponweight_fp16", ctypes.c_int),
        ("clipweight_fp16", ctypes.c_int),
    ]


class DefWeaponsFile(ctypes.Structure):
    _fields_ = [
        ("ammo_class_lines", ctypes.c_void_p),
        ("ammo_class_lines_count", ctypes.c_size_t),
        ("entries", ctypes.POINTER(DefWeaponDef)),
        ("count", ctypes.c_size_t),
    ]


class DefItemParticleFx(ctypes.Structure):
    # Mirror of def.h DefItemParticleFx (one anchored per-item effect slot).
    _fields_ = [
        ("effect", ctypes.c_char * 32),
        ("userpoint", ctypes.c_char * 32),
        ("secondary_effect", ctypes.c_char * 32),
    ]


class DefItemEmplacementAttachment(ctypes.Structure):
    _fields_ = [
        ("userpoint", ctypes.c_char * 16),
        ("item_id", ctypes.c_int),
        ("down_angle", ctypes.c_int),
        ("up_angle", ctypes.c_int),
        ("right_angle", ctypes.c_int),
        ("left_angle", ctypes.c_int),
        ("angle_count", ctypes.c_int),
        ("kind", ctypes.c_int),
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
        ("sound_profile_female", ctypes.c_char * 128),
        ("soundloops", (ctypes.c_char * 128) * 7),
        ("nightshot", ctypes.c_char * 128),
        ("dawnshot", ctypes.c_char * 128),
        ("duskshot", ctypes.c_char * 128),
        ("dayshot", ctypes.c_char * 128),
        # §5.10b dispatch tags + attrib masks + the vehicle physics block (mirror
        # def.h — these were added C-side over several slices; entries[] indexing
        # is stride-sensitive, so every C field must appear here).
        ("ai_function", ctypes.c_char * 16),
        ("move_function", ctypes.c_char * 16),
        ("render_function", ctypes.c_char * 16),
        ("disk_function", ctypes.c_char * 16),
        ("attrib", ctypes.c_uint),
        ("attrib2", ctypes.c_uint),
        ("physics", ctypes.c_int),
        ("acceleration", ctypes.c_int),
        ("deceleration", ctypes.c_int),
        ("player_speed", ctypes.c_int),
        ("water_speed", ctypes.c_int),
        ("slip_speed", ctypes.c_int),
        ("max_slope", ctypes.c_int),
        ("slip_slope", ctypes.c_int),
        ("turn_rate", ctypes.c_int),
        ("turn_rate2", ctypes.c_int),
        # Collision speed-decay shift count ('torque', raw; mirror def.h).
        ("torque", ctypes.c_int),
        ("critical_hp", ctypes.c_int),
        ("critical_drain", ctypes.c_int),
        ("unit_type", ctypes.c_int),
        # Per-item particle-effect keys (mirror def.h; ItemDef_ParseProperty
        # @ 0x49eb00 particlefx family).
        ("particlefx", DefItemParticleFx),
        ("particlefxs", DefItemParticleFx),
        ("particlefxw1", DefItemParticleFx),
        ("particlefxw2", DefItemParticleFx),
        ("particlefxw3", DefItemParticleFx),
        ("particlefxw4", DefItemParticleFx),
        ("particledeath", ctypes.c_char * 32),
        ("particleh2odeath", ctypes.c_char * 32),
        ("particlefire", ctypes.c_char * 32),
        ("particleother", ctypes.c_char * 32),
        ("particlefinale", ctypes.c_char * 32),
        ("particlespawn", ctypes.c_char * 32),
        ("raw_lines", ctypes.c_void_p),
        ("raw_lines_count", ctypes.c_size_t),
        # Person-item firing and lifetime fields appended C-side after the raw
        # source lines; keep them here so entries[] retains the native stride.
        ("ammo_closeattack", ctypes.c_char * 32),
        ("clipsize", ctypes.c_int),
        ("deathtime_ticks", ctypes.c_int),
        # Emplacement weapon link ('primary_weapon'; empty = key absent; mirror def.h).
        ("primary_weapon", ctypes.c_char * 32),
        # The destruction/husk block (mirror def.h; appended for FFI stride stability).
        ("huskfinal", ctypes.c_char * 128),
        ("sounddeath", ctypes.c_char * 32),
        ("armor_impact", ctypes.c_int),
        ("armor_blast", ctypes.c_int),
        ("kz", ctypes.c_float),
        ("husk_swap_at", ctypes.c_float),
        ("husk_swap_at_sec", ctypes.c_float),
        ("debris_scale", ctypes.c_float),
        ("husk_sub_parts", ctypes.c_int),
        ("husk_sub_part_types", ctypes.c_ubyte * 16),
        # Mounted selector source: phrase_set dword + explicit authored presence.
        ("phrase_set", ctypes.c_int),
        ("phrase_set_valid", ctypes.c_int),
        # Projectile/vehicle damage traits appended C-side (mirror def.h). The
        # destruction block's armor_impact is shared; armor_kz mirrors armor_blast.
        ("damage_reduc_pp", ctypes.c_float),
        ("damage_reduc_max", ctypes.c_float),
        ("armor_kz", ctypes.c_int),
        ("emplacement_attachments", ctypes.POINTER(DefItemEmplacementAttachment)),
        ("emplacement_attachments_count", ctypes.c_size_t),
        ("emplacement_g_slot", ctypes.c_int),
        ("emplacement_c_slot", ctypes.c_int),
        # Building-interior daylight fraction (retail ItemDef+0x218).
        ("light_transfer", ctypes.c_float),
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
