class_name HudStance
extends RefCounted

## The stance indicator: discrete pre-rendered frames (the hudpos.def HUDSTANCE set —
## stand/crouch/prone/sitting/emplaced) selected by the player's stance index, each
## scaled to fit a 128 design-pixel box and drawn at HUDSTANCEPOS. The original cross-
## fades when the stance changes. [orig: HUD_DrawStanceIndicator @0x599f10]
##
## NOTE: the IDB curated name for this function ("draw_minimap_compass_overlay") is a
## misnomer — it is the stance widget, NOT a compass (D-HUD-1). The rotating-compass
## look in the oscarmike reference is therefore not faithful (D-HUD-2). The true
## heading/radar is a separate, not-yet-witnessed function (draw_minimap_overlay).

const BOX := 128.0 # design-space box each frame is scaled into [orig: 0x800000 / max(w,h)]


## Draw one stance frame at the anchor (HUDSTANCEPOS, design space), scaled uniformly
## into the 128-px box and centered, then mapped to the surface. `modulate` carries the
## cross-fade alpha / stancecolor.
static func draw(ci: CanvasItem, frame: Texture2D, anchor_design: Vector2, surface: Vector2,
		modulate: Color = Color.WHITE) -> void:
	if ci == null or frame == null:
		return
	var tex := frame.get_size()
	var m := maxf(tex.x, tex.y)
	if m <= 0.0:
		return
	var scale := BOX / m
	var scaled := tex * scale
	var offset := Vector2((BOX - scaled.x) * 0.5, (BOX - scaled.y) * 0.5)
	var dst := HudLayout.scale_rect(Rect2(anchor_design + offset, scaled), surface)
	ci.draw_texture_rect(frame, dst, false, modulate)
