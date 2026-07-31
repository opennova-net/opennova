extends Control

# EngineTextPreview (maturity slice F2): one widget that renders a sample line
# through the SAME seams the game runtime draws text with — NovaFntResource ->
# FontFile (the .fnt parser view menus/credits text rides) and HudText.draw_text
# (design-space anchor scaling [orig: Viewport_ScaleToVirtualCoords @0x5d2b20],
# baseline/ascent placement, alignment, half-bright
# [orig: HUD_DrawTextRightAligned_HalfBright @0x580850 -> CGameFont_DrawText @0x6752c0]).
# It replaces nothing: it gives Fonts/Strings/HUD a truthful "how the game draws
# it" panel beside their authoring surfaces (FNT-1 / STR-1 / HUD-3 adopt it).
#
# The anchor POSITION inside the panel is an owner choice (kept mid-panel by
# alignment); everything from the anchor onward — scaling, ascent offset,
# alignment shift, fixed pixel size, half-bright — is the engine path.
#
# Reference via preload(), not class_name, so it resolves without an editor
# re-import (same convention as world_context_preview.gd / mission_object_placer.gd).

const BACKGROUND_COLOR := Color(0.08, 0.09, 0.10, 1.0)
const BORDER_COLOR := Color(0.2, 0.22, 0.25, 1.0)
const EMPTY_HINT := "Load a font to preview the game draw."

# Design-space x anchors per alignment (hudpos-style authoring margins).
const LEFT_MARGIN_X := 16.0
const RIGHT_MARGIN_X := 1008.0

var _font: FontFile
var _sample_text := "The quick brown fox 0123456789"
var _text_color := Color(0.98, 0.84, 0.02) # hud_textcolor default family
var _half_bright := false
var _align := int(HudText.Align.LEFT)


func set_sample_text(text: String) -> void:
	_sample_text = text
	queue_redraw()


func get_sample_text() -> String:
	return _sample_text


func has_font() -> bool:
	return _font != null


func get_font_file() -> FontFile:
	return _font


func set_font_file(font: FontFile) -> void:
	_font = font
	queue_redraw()


## Adopt the edited document's font (the Fonts workspace case). A null or
## unloadable resource clears the panel and reports false.
func set_font_from_fnt(fnt: NovaFntResource) -> bool:
	if fnt == null:
		set_font_file(null)
		return false
	var font := fnt.to_font_file()
	set_font_file(font)
	return font != null


## Resolve a font by name from the mounted resource root, the way the runtime
## does (HudText.load_font -> VFS read -> NovaFntResource). A failed load keeps
## the current font (a typo must not blank the panel) and reports false.
func set_font_from_root(root: NovaResourceRoot, font_name: String) -> bool:
	var font := HudText.load_font(root, font_name)
	if font == null:
		return false
	set_font_file(font)
	return true


func set_text_color(color: Color) -> void:
	_text_color = color
	queue_redraw()


func set_half_bright(enabled: bool) -> void:
	_half_bright = enabled
	queue_redraw()


func is_half_bright() -> bool:
	return _half_bright


func set_alignment(align: int) -> void:
	_align = align
	queue_redraw()


func get_alignment() -> int:
	return _align


## The exact color handed to the engine draw (the half-bright mode is the
## original's (color >> 1) | opaque-alpha, via HudText.half_bright).
func effective_color() -> Color:
	return HudText.half_bright(_text_color) if _half_bright else _text_color


## The fixed pixel size the draw uses — the same resolution HudText.draw_text
## applies (.fnt bitmap fonts carry one fixed size; positions scale, glyphs
## do not).
func font_pixel_size() -> int:
	if _font != null:
		var fixed := _font.get_fixed_size()
		if fixed > 0:
			return fixed
	return 16


## The design-space (1024x768) anchor the sample is drawn at: x from the
## alignment (left margin / center / right margin), y chosen so the glyph cell
## sits mid-panel after HudLayout scaling.
func draw_anchor_design() -> Vector2:
	var x := LEFT_MARGIN_X
	if _align == int(HudText.Align.CENTER):
		x = HudLayout.DESIGN_WIDTH * 0.5
	elif _align == int(HudText.Align.RIGHT):
		x = RIGHT_MARGIN_X
	var y := HudLayout.DESIGN_HEIGHT * 0.5
	if size.y > 0.0:
		y = HudLayout.DESIGN_HEIGHT * (size.y - float(font_pixel_size())) / (2.0 * size.y)
	return Vector2(x, y)


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		queue_redraw()


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BACKGROUND_COLOR, true)
	draw_rect(Rect2(Vector2.ZERO, size), BORDER_COLOR, false, 1.0)
	if _font == null:
		var f := ThemeDB.fallback_font
		var fs := ThemeDB.fallback_font_size
		draw_string(f, Vector2(8, size.y * 0.5 + fs * 0.35), EMPTY_HINT,
			HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0.6, 0.6, 0.65))
		return
	HudText.draw_text(self, _font, draw_anchor_design(), size, _sample_text,
		effective_color(), _align)
