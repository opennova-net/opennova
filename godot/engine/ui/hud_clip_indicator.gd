class_name HudClipIndicator
extends RefCounted

## The clip/rounds graphic at HUDCLIP: the weapon's HUDCLIPGFX background plus one
## HUDRNDGFX icon per remaining round, stepped along the weapon's per-round vector,
## all tinted STANCEICON_COLOR. On an ammo-state change the round icons flash toward
## full alpha and decay back to the ALPHAFADE base. Stateful (the change stamp), so
## hosts keep one instance per HUD. [orig: draw_hud_ammo_indicator @0x599a30]

const MAX_ROUND_ICONS := 40 # [orig: @0x599bbf]

# Flash restamp state. The original keys the stamp on (ammo class def+220, reserve,
# pool id def+216); our single-pool weapon model keys on (round_type, reserve) — the
# same reload/switch transitions at the reimpl's pool altitude
# (docs/interface/hud-re.md D-HUD-5; the pool model is net-re D-WPN-2).
var _key := ""
var _stamp_ticks := 0


## How many round icons: the magazine count (the carried pool for capacity-1 weapons),
## divided by the rounds-per-icon divisor when > 1 (rounded up), capped at 40.
## [orig: @0x599b9c..0x599bc1]
static func round_icon_count(clip: int, reserve: int, capacity: int, divisor: int) -> int:
	var n := reserve if capacity == 1 else clip
	if divisor > 1:
		n = (n + 1) / divisor
	return mini(n, MAX_ROUND_ICONS)


func reset() -> void:
	_key = ""
	_stamp_ticks = 0


## `weapon` is the equipped weapon's HUD slice (PlayerHudWeaponDef). `anchor_design`
## is the hudpos HUDCLIP position; either component nonzero enables the element.
## `fade` is the ALPHAFADE triple (base%, max%, seconds — raw file fields).
## [orig gates: @0x599a4a anchor, @0x599a59 ramp, @0x599a82 -1 sentinels]
func draw(ci: CanvasItem, anchor_design: Vector2i, weapon: PlayerHudWeaponDef,
		clip_tex: Texture2D, round_tex: Texture2D, clip: int, reserve: int,
		tint: Color, fade: Vector3, now_ticks: int, surface: Vector2) -> void:
	if ci == null or weapon == null or anchor_design == Vector2i.ZERO:
		return
	var ramp_ticks := int(fade.z * HudFade.SECONDS_TO_TICKS)
	if ramp_ticks <= 0:
		return
	if reserve == -1 or clip == -1:
		return

	var key := "%s|%d" % [weapon.round_type, reserve]
	if key != _key:
		_key = key
		_stamp_ticks = now_ticks
	var base_alpha := int(fade.x * HudFade.PERCENT_TO_ALPHA)
	var max_alpha := int(fade.y * HudFade.PERCENT_TO_ALPHA)
	var flash := HudFade.flash_alpha(now_ticks - _stamp_ticks, ramp_ticks, base_alpha, max_alpha)

	var anchor := Vector2(anchor_design)
	if clip_tex != null:
		var bg_color := Color(tint.r, tint.g, tint.b, base_alpha / 255.0)
		ci.draw_texture_rect(clip_tex,
			HudLayout.scale_rect(Rect2(anchor + Vector2(weapon.clipgfx_offset), clip_tex.get_size()), surface),
			false, bg_color)

	if round_tex != null:
		var n := round_icon_count(clip, reserve, weapon.clipsize, weapon.rounds_per_icon)
		var round_color := Color(tint.r, tint.g, tint.b, flash / 255.0)
		var pos := anchor + Vector2(weapon.rndgfx_offset)
		var step := Vector2(weapon.rndgfx_step)
		var round_size := round_tex.get_size()
		for i in n:
			ci.draw_texture_rect(round_tex,
				HudLayout.scale_rect(Rect2(pos, round_size), surface), false, round_color)
			pos += step
