class_name PlayerViewmodelDef
extends RefCounted

## The weapon.def slice driving the first-person viewmodel, decoded from the
## WeaponDef record `WeaponDatabase.get_weapon()` returns. Model names resolve the gun
## and the shared animation set (the first-person ARMS are the local player's
## character arms, never a weapon.def field: retail parses-and-discards gfx1a/gfx1b
## [orig: WeaponDefs_ParseLineCallback @0x5448d0/@0x5448e6 -> loc_545098]);
## `pos_units`/`tpos_units` are the RAW def units
## (/256 = world; the hip and ADS view biases), `rot_bias_deg` the def's yaw/pitch/roll
## degrees added to the view angles, `renderfov_h_deg` the FP projection's HORIZONTAL
## fov (record default 80.0 — no shipped JO def sets the key).
## [orig: WeaponDef_ParseProperty @0x54d730; pos/tpos handlers @0x54476b/@0x54471f;
## renderfov @0x54482a, default flt_7D1898 @0x53ff31]

var weapon_name := ""
var gfx1 := ""       # the FP gun model (.3di basename)
var gfx3 := ""       # the THIRD-person world gun, drawn in the soldier's hands
                     # [orig: WeaponDef.tpModel +0x170, read @0x4e3cd3. NOTE the IDB
                     #  locals in WeaponDef_ResolveAllReferences @0x54042c are swapped:
                     #  `model_1p` there reads +0x170, which is THIS field.]
var animadm := ""    # the shared animation set (.adm basename)
var pos_units := Vector3.ZERO       # hip view bias, raw def units
var rot_bias_deg := Vector3.ZERO    # def rot columns: yaw/pitch/roll degrees
var tpos_units := Vector3.ZERO      # ADS view bias [orig: WeaponDef.CamOffsetTpos @0x124]
# Record default = the engine base camera fov (world/player_view.h
# kPlayerCameraFovHDeg, Simulation.DEFAULT_PLAYER_FOV_H_DEG)
# [orig: renderfov default flt_7D1898 @0x53ff31].
var renderfov_h_deg: float = Simulation.DEFAULT_PLAYER_FOV_H_DEG
# The witnessed WeaponDef+8 flag mask (file tokens: scoped 1, sighted 2, burst 0x20,
# auto 0x100) — Flags & 3 gates the ADS toggle [orig: Player_ToggleWeaponScope
# @0x4df0c0] — and the ADS zoom magnification (scoped camera FOV = 80 / zoom
# [orig: @0x4df401]; 0 = key absent, no zoom change).
var flags := 0
var scope_max_mag := 0.0
# Magazine size (0 = no clipsize key -> the FSM tracks no clip); the reload key is
# refused on a full magazine [orig: the reload input case @0x4e0420 compares the clip
# against clipsize before WeaponSlot_RequestReload].
var clipsize := 0


## Decode one WeaponDef; null for no weapon (weapon.def or the weapon name
## unresolved — callers keep their built-in fallbacks).
static func from_weapon_def(def: WeaponDef) -> PlayerViewmodelDef:
	if def == null:
		return null
	var out := PlayerViewmodelDef.new()
	out.weapon_name = def.name
	out.gfx1 = def.gfx1
	out.gfx3 = def.gfx3
	out.animadm = def.animadm
	var pos := def.pos
	out.pos_units = Vector3(pos[0], pos[1], pos[2])
	out.rot_bias_deg = Vector3(pos[3], pos[4], pos[5])
	var tpos := def.tpos
	out.tpos_units = Vector3(tpos[0], tpos[1], tpos[2])
	out.renderfov_h_deg = def.renderfov
	out.flags = def.flags
	out.scope_max_mag = def.scope_max_mag
	out.clipsize = def.clipsize
	return out
