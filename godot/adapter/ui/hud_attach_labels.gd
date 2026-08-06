class_name HudAttachLabels
extends RefCounted

## The floating seat/armory attach labels: a wireframe box + centered text at each
## eligible attach point's PROJECTED screen position — raw screen pixels, not the
## 1024x768 design space (the original projects and draws at screen coordinates).
## The nearest candidate draws full color; every other label draws dimmed.
## Metrics/color residuals: docs/interface/hud-re.md (D-HUD-12, D-HUD-13).
## [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 — measure HUD_MeasureTextWH
##  @0x5a3680, box Render_DrawWireframeRect @0x5a36ad, text
##  HUD_DrawTextCentered_HalfBright @0x5a36c1]

## The non-nearest label color: RGB halved, alpha forced to 0x7F.
## [orig: ((rgb & 0xFEFEFE) | 0xFE000001) >> 1 @0x5a364e]
static func dim(c: Color) -> Color:
	return Color(c.r * 0.5, c.g * 0.5, c.b * 0.5, 127.0 / 255.0)


## One label at a projected screen point. Box geometry verbatim:
## (x - w/2, y - 2) .. (x + w/2 + 5, y + h + 1); the text centered on x from y.
## The text rides the half-bright color mode like every HUD text draw; the box
## takes the color raw. [orig: @0x5a36ad/@0x5a36c1]
static func draw(ci: CanvasItem, font: Font, screen: Vector2, text: String,
		color: Color) -> void:
	if ci == null or font == null:
		return
	var fs := 16
	if font is FontFile:
		fs = (font as FontFile).get_fixed_size()
	var w := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
	var h := font.get_height(fs)
	ci.draw_rect(Rect2(screen.x - w * 0.5, screen.y - 2.0, w + 5.0, h + 3.0),
		color, false, 1.0)
	if not text.is_empty():
		ci.draw_string(font, Vector2(screen.x - w * 0.5, screen.y + font.get_ascent(fs)),
			text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, HudText.half_bright(color))
