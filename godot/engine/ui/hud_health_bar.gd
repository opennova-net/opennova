class_name HudHealthBar
extends RefCounted

## The health bar: a border rectangle plus a fill proportional to remaining health,
## drawn left-to-right and colored by threshold. [orig: HUD_DrawHealthBar @0x5a2e50]
##
## `fraction` is the witnessed 16.16 health ratio expressed as 0..1. Color thresholds:
## > 0.75 (0xC000) good, > ~0.437 (0x6FFF) mid, else bad. An all-zero rect means the
## element is disabled (the original early-returns). Shell-neutral.

const GOOD_THRESHOLD := 49152.0 / 65536.0 # 0xC000
const MID_THRESHOLD := 28671.0 / 65536.0  # 0x6FFF


static func draw(ci: CanvasItem, design_rect: Rect2, fraction: float, border: Color,
		good: Color, mid: Color, bad: Color, surface: Vector2) -> void:
	if ci == null:
		return
	if design_rect.position == Vector2.ZERO and design_rect.size == Vector2.ZERO:
		return # all-zero => element disabled

	var r := HudLayout.scale_rect(design_rect, surface)
	var f := clampf(fraction, 0.0, 1.0)
	var fill_color := bad
	if f > GOOD_THRESHOLD:
		fill_color = good
	elif f > MID_THRESHOLD:
		fill_color = mid

	# Fill from the left edge, 1px inset. [orig: sub_5D48E0 (x1+1,y1+1)..(x1+fill,y2)]
	var fill_w := r.size.x * f
	if fill_w > 1.0:
		ci.draw_rect(Rect2(r.position + Vector2(1, 1),
			Vector2(fill_w - 1.0, maxf(r.size.y - 1.0, 0.0))), fill_color, true)
	# Border on top (the original draws a wireframe rect).
	ci.draw_rect(r, border, false)
