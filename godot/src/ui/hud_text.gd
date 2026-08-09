class_name HudText
extends RefCounted

## .fnt text helpers for ONED's preview surfaces (HUD layout preview, engine
## text preview, strings workspace): load a font named in hudpos.def
## (font_hi/font_lo) through FntResource's Godot FontFile view and draw it with
## design-space scaling and the half-bright color mode.
## [orig: HUD_DrawTextRightAligned_HalfBright @0x580850 -> CGameFont_DrawText @0x6752c0]
##
## The RUNTIME HUD no longer draws through this: the engine GameFont
## (engine/runtime/hud) lays out glyph quads exactly and HudOverlay rasterizes
## them — this FontFile approximation stays only where an editor preview wants
## a Godot Font object. Callers should load fonts once (not per frame).

enum Align { LEFT = 0, RIGHT = 1, CENTER = 2 }


## Load a hudpos.def font name (e.g. "Gunpb18b.fnt") through the VFS as a Godot FontFile.
## Returns null if the name is empty or unresolvable.
static func load_font(root: ResourceRoot, font_name: String) -> FontFile:
	if root == null or font_name.is_empty():
		return null
	var bytes := root.read_file(font_name.get_file())
	if bytes.is_empty():
		return null
	var res := FntResource.new()
	if res.load_from_bytes(bytes) != OK:
		return null
	return res.to_font_file()


## Half-bright: halve each RGB channel and force opaque alpha.
## [orig: (color >> 1) & 0x7F7F7F | 0xFF000000]
static func half_bright(c: Color) -> Color:
	return HudPos.half_bright(c)


## Draw text at a design-space position scaled onto the surface, with alignment.
static func draw_text(ci: CanvasItem, font: Font, design_pos: Vector2, surface: Vector2,
		text: String, color: Color, align: int = Align.LEFT, font_size: int = -1) -> void:
	if ci == null or font == null or text.is_empty():
		return
	var fs := font_size
	if fs <= 0:
		fs = 16
		if font is FontFile:
			fs = (font as FontFile).get_fixed_size()
	var pos := HudPos.scale_point(design_pos, surface)
	var width := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
	if align == Align.RIGHT:
		pos.x -= width
	elif align == Align.CENTER:
		pos.x -= width * 0.5
	# hudpos.def text positions are top-ish; draw_string draws from the baseline, so
	# offset down by the ascent to place the glyph cell at the design position.
	ci.draw_string(font, Vector2(pos.x, pos.y + font.get_ascent(fs)), text,
		HORIZONTAL_ALIGNMENT_LEFT, -1, fs, color)
