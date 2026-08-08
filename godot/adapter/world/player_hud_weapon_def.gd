class_name PlayerHudWeaponDef
extends RefCounted

## The weapon.def slice the HUD's weapon-coupled elements read — the typed record
## behind `NovaWeaponDatabase.get_weapon()`'s transport Dictionary (ADR 0017: the
## record is the contract, the dict is its C++-binding encoding). Mirrors the
## original's per-frame HUD info struct holding the equipped weapon-def pointer
## [orig: HUD_BuildEntityInfo @0x4b8561 -> hudInfo+552].
##
## `error_deg` is the 6-row dispersion table in DEGREES — rows hip prone/crouch/stand
## then scoped prone/crouch/stand; the crosshair spread reads row stance + 3*scoped
## [orig: parse @0x543b21 (16.16); HUD_DrawCrosshair @0x592b84]. The clip graphic
## (HUDCLIPGFX [orig: parse @0x54427f]) and the per-round row (HUDRNDGFX
## [orig: parse @0x5442fc -> weapon +644..+656, divisor byte +727]) drive the
## clip indicator.

var weapon_name := ""
var round_type := ""            # ammo key — the clip-flash restamp proxy (D-HUD-5)
var clipsize := 0               # magazine capacity; -1 = infinite (clip reads -1)
var error_deg := PackedFloat32Array()
var clipgfx_texture := ""
var clipgfx_offset := Vector2i.ZERO
var rndgfx_texture := ""
var rndgfx_offset := Vector2i.ZERO  # first round icon, relative to the HUDCLIP anchor
var rndgfx_step := Vector2i.ZERO    # per-round icon step
var rounds_per_icon := 0            # >1 draws one icon per N rounds, rounded up
# Standard SIGHTS-card contents. The simulation owns the dynamic card selector;
# this record only preserves every authored row as a dict
# {texture,x1,y1,x2,y2,blend,scale,slide,slide_frames} in draw order and virtual
# 1024x768 space. [orig: draw_weapon_sight_overlays @0x4dce00]
var sights: Array = []


## The dispersion row in degrees; 0.0 outside the parsed table.
func error_row_deg(row: int) -> float:
	return error_deg[row] if row >= 0 and row < error_deg.size() else 0.0


## Decode one NovaWeaponDatabase weapon dict; null when the dict is empty (no weapon
## resolved — the HUD then draws no weapon cluster).
static func from_weapon_dict(d: Dictionary) -> PlayerHudWeaponDef:
	if d.is_empty():
		return null
	var out := PlayerHudWeaponDef.new()
	out.weapon_name = String(d.get("name", ""))
	out.round_type = String(d.get("round_type", ""))
	out.clipsize = int(d.get("clipsize", 0))
	out.error_deg = d.get("error", PackedFloat32Array())
	out.clipgfx_texture = String(d.get("hudclipgfx_texture", ""))
	out.clipgfx_offset = d.get("hudclipgfx_offset", Vector2i.ZERO)
	out.rndgfx_texture = String(d.get("hudrndgfx_texture", ""))
	out.rndgfx_offset = d.get("hudrndgfx_offset", Vector2i.ZERO)
	var layout: Vector3i = d.get("hudrndgfx_layout", Vector3i.ZERO)
	out.rndgfx_step = Vector2i(layout.x, layout.y)
	# The original stores the divisor as a byte (weapon+727) — out-of-range
	# file values wrap mod 256. [orig: HUDRNDGFX parse @0x5442fc; read @0x599bb1]
	out.rounds_per_icon = layout.z & 0xFF
	out.sights = d.get("sights", [])
	return out
