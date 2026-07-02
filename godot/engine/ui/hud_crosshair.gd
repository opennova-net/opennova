class_name HudCrosshair
extends RefCounted

## The original reticle: one crosshair texture split into 5 regions — top/bottom/left/
## right arms plus a center dot — that spread outward with weapon error. The texture
## atlas is split at UV 0.0 / 0.45 / 0.5 / 0.55 / 1.0 (arms in the outer bands, the
## center in the 0.45..0.55 band). With spread 0 the regions assemble the full reticle
## at center. [orig: HUD_DrawCrosshair @0x592640 -> HUD_DrawCrosshairCornerQuad @0x590f50]
##
## The spread amount is weapon/recoil-driven and is deferred with the weapon model;
## a static reticle (spread 0) is faithful and complete for the on-foot HUD.
## NOTE: the original tapers each arm by 0.1 (a tri-strip quad); this rect-region port
## omits the taper (see docs/interface/hud-re.md follow-ups).

const ARM_UV := 0.45 # outer arms occupy 0..0.45 and 0.55..1.0; center is the 0.1 band


static func draw(ci: CanvasItem, texture: Texture2D, center: Vector2, spread_px: float = 0.0) -> void:
	if ci == null or texture == null:
		return
	var tex := texture.get_size()
	var half := tex * 0.5
	if spread_px < 2.0:
		# Static: the whole reticle meets at center.
		ci.draw_texture_rect(texture, Rect2(center - half, tex), false)
		return

	var arm_v := tex.y * ARM_UV
	var arm_h := tex.x * ARM_UV
	var center_sz := tex * (1.0 - 2.0 * ARM_UV)

	# TOP arm (UV top band), pushed up by spread.
	ci.draw_texture_rect_region(texture,
		Rect2(center.x - half.x, center.y - spread_px - arm_v, tex.x, arm_v),
		Rect2(0, 0, tex.x, arm_v))
	# BOTTOM arm (UV bottom band), pushed down.
	ci.draw_texture_rect_region(texture,
		Rect2(center.x - half.x, center.y + spread_px, tex.x, arm_v),
		Rect2(0, tex.y - arm_v, tex.x, arm_v))
	# LEFT arm (UV left band), pushed left.
	ci.draw_texture_rect_region(texture,
		Rect2(center.x - spread_px - arm_h, center.y - half.y, arm_h, tex.y),
		Rect2(0, 0, arm_h, tex.y))
	# RIGHT arm (UV right band), pushed right.
	ci.draw_texture_rect_region(texture,
		Rect2(center.x + spread_px, center.y - half.y, arm_h, tex.y),
		Rect2(tex.x - arm_h, 0, arm_h, tex.y))
	# CENTER dot: always at center regardless of spread. [orig: case 4]
	ci.draw_texture_rect_region(texture,
		Rect2(center - center_sz * 0.5, center_sz),
		Rect2(tex * ARM_UV, center_sz))
