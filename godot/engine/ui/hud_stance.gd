class_name HudStance
extends RefCounted

## The stance indicator: discrete pre-rendered frames (the hudpos.def HUDSTANCE set —
## stand/crouch/prone/sitting/emplaced/parachute) selected by the player's stance
## index, drawn at HUDSTANCEPOS plus the frame's own HUDSTANCE offset. On a stance
## change the original cross-fades: the new frame flashes toward full alpha decaying
## to the ALPHAFADE base while the previous frame ghosts on top at quarter fade
## (HudFade.stance_*); both tinted STANCEICON_COLOR.
##
## Scaling is SHARED from frame 0: one 16.16 factor 0x800000/max(w0,h0) scales every
## frame's dims, and the 128-box centering offset comes from frame 0's scaled dims
## (no centering at 127+). [orig: HUD_DrawStanceIndicator @0x599f10 — shared scale
## @0x599fed..0x59a00a, centering @0x59a02a..0x59a07e, anchor+offset @0x59a173]
##
## NOTE: the IDB curated name for this function ("draw_minimap_compass_overlay") is a
## misnomer — it is the stance widget, NOT a compass (D-HUD-1). The rotating-compass
## look in the oscarmike reference is therefore not faithful (D-HUD-2). The true
## heading/radar is a separate, not-yet-witnessed function (draw_minimap_overlay).


## The shared 16.16 scale factor from FRAME 0's dims — the original computes ONE
## factor and every frame (team tile / current / previous) scales by it.
## [orig: 0x800000 / max(w,h) @0x59a00a]
static func scale_q16(frame0_size: Vector2i) -> int:
	var m := maxi(frame0_size.x, frame0_size.y)
	return 0x800000 / m if m > 0 else 0


## One dimension through the witnessed fixed-point scale. [orig: @0x59a023]
static func scaled_dim(dim: int, q16: int) -> int:
	return (q16 * dim + 0x8000) >> 16


## The shared centering offset from frame 0's scaled dims; 127+ centers at 0.
## [orig: @0x59a02a..0x59a07e]
static func center_offset(frame0_size: Vector2i, q16: int) -> Vector2i:
	var sw := scaled_dim(frame0_size.x, q16)
	var sh := scaled_dim(frame0_size.y, q16)
	return Vector2i(
		0 if sw >= 127 else (128 - sw) / 2,
		0 if sh >= 127 else (128 - sh) / 2)


## Draw one stance frame at the anchor (HUDSTANCEPOS, design space) plus the frame's
## HUDSTANCE offset plus the shared centering offset, its dims through the shared
## frame-0 scale, mapped to the surface. `modulate` carries the cross-fade alpha over
## the STANCEICON_COLOR tint. `frame0_size` defaults to this frame's own size
## (single-frame callers are their own frame 0).
static func draw(ci: CanvasItem, frame: Texture2D, anchor_design: Vector2, surface: Vector2,
		modulate: Color = Color.WHITE, frame_offset: Vector2 = Vector2.ZERO,
		frame0_size: Vector2i = Vector2i.ZERO) -> void:
	if ci == null or frame == null:
		return
	var tex := Vector2i(frame.get_size())
	var basis := frame0_size if frame0_size != Vector2i.ZERO else tex
	var q16 := scale_q16(basis)
	if q16 <= 0:
		return
	var scaled := Vector2(scaled_dim(tex.x, q16), scaled_dim(tex.y, q16))
	var offset := Vector2(center_offset(basis, q16))
	var dst := HudLayout.scale_rect(Rect2(anchor_design + frame_offset + offset, scaled), surface)
	ci.draw_texture_rect(frame, dst, false, modulate)
