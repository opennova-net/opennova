class_name PlayerHudWeaponDef
extends RefCounted

## The weapon.def slice the HUD's weapon-coupled elements read, decoded from
## the WeaponDef record `WeaponDatabase.get_weapon()` returns. Mirrors the
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
# this record only preserves every authored row (WeaponSightRow) in draw order
# and virtual 1024x768 space. [orig: draw_weapon_sight_overlays @0x4dce00]
var sights: Array[WeaponSightRow] = []


## The dispersion row in degrees; 0.0 outside the parsed table.
func error_row_deg(row: int) -> float:
	return error_deg[row] if row >= 0 and row < error_deg.size() else 0.0


## Decode one WeaponDef; null for no weapon (the HUD then draws no weapon
## cluster).
static func from_weapon_def(def: WeaponDef) -> PlayerHudWeaponDef:
	if def == null:
		return null
	var out := PlayerHudWeaponDef.new()
	out.weapon_name = def.name
	out.round_type = def.round_type
	out.clipsize = def.clipsize
	out.error_deg = def.error
	out.clipgfx_texture = def.hudclipgfx_texture
	out.clipgfx_offset = def.hudclipgfx_offset
	out.rndgfx_texture = def.hudrndgfx_texture
	out.rndgfx_offset = def.hudrndgfx_offset
	var layout := def.hudrndgfx_layout
	out.rndgfx_step = Vector2i(layout.x, layout.y)
	out.rounds_per_icon = rounds_per_icon_from_layout(layout)
	out.sights = def.get_sights()
	return out


## The HUDRNDGFX divisor as the original reads it: stored as a byte
## (weapon+727), so an out-of-range file value wraps mod 256.
## [orig: HUDRNDGFX parse @0x5442fc; read @0x599bb1]
static func rounds_per_icon_from_layout(layout: Vector3i) -> int:
	return layout.z & 0xFF
