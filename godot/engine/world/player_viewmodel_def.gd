class_name PlayerViewmodelDef
extends RefCounted

## The weapon.def slice driving the first-person viewmodel — the typed record behind
## `NovaWeaponDatabase.get_weapon()`'s transport Dictionary (ADR 0017: the record is
## the contract, the dict is its C++-binding encoding). Model names resolve the gun,
## arms, and shared animation set; `pos_units`/`tpos_units` are the RAW def units
## (/256 = world; the hip and ADS view biases), `rot_bias_deg` the def's yaw/pitch/roll
## degrees added to the view angles, `renderfov_h_deg` the FP projection's HORIZONTAL
## fov (record default 80.0 — no shipped JO def sets the key).
## [orig: WeaponDef_ParseProperty @0x54d730; pos/tpos handlers @0x54476b/@0x54471f;
## renderfov @0x54482a, default flt_7D1898 @0x53ff31]

var weapon_name := ""
var gfx1 := ""       # the FP gun model (.3di basename)
var gfx1a := ""      # the character arms riding the gun's skeleton
var gfx1b := ""      # alternate arm skin (unused until team/skin selection)
var animadm := ""    # the shared animation set (.adm basename)
var pos_units := Vector3.ZERO       # hip view bias, raw def units
var rot_bias_deg := Vector3.ZERO    # def rot columns: yaw/pitch/roll degrees
var tpos_units := Vector3.ZERO      # ADS view bias [orig: WeaponDef.AltCamOffset @0x10C]
var renderfov_h_deg := 80.0


## Decode one NovaWeaponDatabase weapon dict; null when the dict is empty (weapon.def
## or the weapon name unresolved — callers keep their built-in fallbacks).
static func from_weapon_dict(d: Dictionary) -> PlayerViewmodelDef:
	if d.is_empty():
		return null
	var out := PlayerViewmodelDef.new()
	out.weapon_name = String(d.get("name", ""))
	out.gfx1 = String(d.get("gfx1", ""))
	out.gfx1a = String(d.get("gfx1a", ""))
	out.gfx1b = String(d.get("gfx1b", ""))
	out.animadm = String(d.get("animadm", ""))
	var pos: PackedFloat32Array = d.get("pos", PackedFloat32Array())
	if pos.size() >= 6:
		out.pos_units = Vector3(pos[0], pos[1], pos[2])
		out.rot_bias_deg = Vector3(pos[3], pos[4], pos[5])
	var tpos: PackedFloat32Array = d.get("tpos", PackedFloat32Array())
	if tpos.size() >= 3:
		out.tpos_units = Vector3(tpos[0], tpos[1], tpos[2])
	out.renderfov_h_deg = float(d.get("renderfov", 80.0))
	return out
