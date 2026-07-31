class_name HudLayoutPreview
extends Control

## Read-only visual preview of a hudpos.def layout for the ONED HUD workspace.
##
## Draws each HUD element at its hudpos design-space position (scaled to this
## control's rect via the shared HudLayout) using the shell-neutral HUD helpers and
## stubbed live values. Real element art (HUD frame, stance frame) is loaded
## best-effort from the mounted VFS; missing art degrades to labeled placeholder
## boxes so the layout is always legible. The model is the witnessed original —
## see docs/interface/hud-re.md.

var _hudpos: NovaHudPos
var _root: NovaResourceRoot
var _font: FontFile
var _stance_tex: Texture2D
var _frame_tex: Texture2D

# Stubbed live values for the preview.
const PREVIEW_HEALTH := 0.6
const PREVIEW_STANCE := 0 # STAND


func set_source(hudpos: NovaHudPos, root: NovaResourceRoot) -> void:
	_hudpos = hudpos
	_root = root
	_font = null
	_stance_tex = null
	_frame_tex = null
	_load_assets()
	queue_redraw()


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		queue_redraw()


func _load_assets() -> void:
	if _hudpos == null or not _hudpos.is_loaded():
		return
	var font_name := _hudpos.get_font_hi()
	if font_name.is_empty():
		font_name = _hudpos.get_font_lo()
	_font = HudText.load_font(_root, font_name)

	var frames := _hudpos.get_static_frames()
	if frames.size() > 0:
		_frame_tex = _load_texture(String((frames[0] as Dictionary).get("texture", "")))

	var stances := _hudpos.get_stances()
	if PREVIEW_STANCE < stances.size():
		_stance_tex = _load_texture(String((stances[PREVIEW_STANCE] as Dictionary).get("texture", "")))


# Best-effort VFS texture load (HUD art ships as .tga). Null on any miss.
func _load_texture(name: String) -> Texture2D:
	if _root == null or name.is_empty():
		return null
	var bytes := _root.read_file(name.get_file())
	if bytes.is_empty():
		return null
	var img := Image.new()
	if img.load_tga_from_buffer(bytes) != OK:
		return null
	return ImageTexture.create_from_image(img)


func _draw() -> void:
	var surface := size
	draw_rect(Rect2(Vector2.ZERO, surface), Color(0.08, 0.09, 0.10, 1.0), true)

	if _hudpos == null or not _hudpos.is_loaded():
		_draw_label(surface * 0.5 - Vector2(150, 0), "Open a hudpos.def to preview the HUD layout.",
			Color(0.6, 0.6, 0.65))
		return

	# Design-space frame border (the 1024x768 bounds fill this control).
	draw_rect(Rect2(Vector2.ZERO, surface), Color(0.2, 0.22, 0.25), false, 1.0)

	var colors := _hudpos.get_colors()

	# Static HUD frame art.
	if _frame_tex != null:
		var frames := _hudpos.get_static_frames()
		if frames.size() > 0:
			var fpos: Vector2i = (frames[0] as Dictionary).get("pos", Vector2i.ZERO)
			var fdst := HudLayout.scale_rect(Rect2(fpos, _frame_tex.get_size()), surface)
			draw_texture_rect(_frame_tex, fdst, false, Color(1, 1, 1, 0.85))

	# Health bar (witnessed thresholds + border).
	var border: Color = colors.get("health_border", Color(0.35, 0.78, 0.78, 0.78))
	var good: Color = colors.get("tagcolor_good", Color(0.02, 0.98, 0.05))
	var mid: Color = colors.get("tagcolor_middle", Color(0.98, 0.65, 0.03))
	var bad: Color = colors.get("tagcolor_bad", Color(0.69, 0.04, 0.04))
	HudHealthBar.draw(self, Rect2(_hudpos.get_health_rect()), PREVIEW_HEALTH, border, good, mid, bad, surface)

	# Stance indicator (the witnessed widget at HUDSTANCEPOS — not a compass).
	var stance_anchor := Vector2(_hudpos.get_stance_pos())
	if stance_anchor != Vector2.ZERO:
		if _stance_tex != null:
			HudStance.draw(self, _stance_tex, stance_anchor, surface)
		else:
			_draw_placeholder(stance_anchor, Vector2(44, 44), surface, "STANCE")

	# Radar / spinmap bounds (the heading map area; draw its frame).
	var spin := _hudpos.get_spinmap_bounds()
	if spin.size != Vector2i.ZERO:
		var sd := HudLayout.scale_rect(Rect2(spin), surface)
		draw_rect(sd, Color(0.3, 0.7, 0.3, 0.5), false, 1.0)
		_draw_label(sd.position + Vector2(3, 13), "RADAR", Color(0.3, 0.7, 0.3, 0.8))

	# Text element positions, with representative samples.
	var text_color: Color = colors.get("hud_textcolor", Color(0.98, 0.84, 0.02))
	var positions: Dictionary = _hudpos.to_dictionary().get("positions", {})
	_draw_text_element(positions, "ammo_count", "30 / 90", text_color, surface, HudText.Align.RIGHT)
	_draw_text_element(positions, "weapon_name", "WEAPON", text_color, surface, HudText.Align.RIGHT)
	_draw_text_element(positions, "time_clock", "12:00", text_color, surface)
	_draw_text_element(positions, "map_coords", "12.3 / 45.6", text_color, surface)
	_draw_text_element(positions, "game_info", "Objective", text_color, surface)
	_draw_text_element(positions, "clip", "CLIP", text_color, surface)

	# Reticle center marker (the crosshair is weapon-driven, not a hudpos element).
	var center := surface * 0.5
	draw_line(center - Vector2(7, 0), center + Vector2(7, 0), Color(1, 1, 1, 0.6), 1.0)
	draw_line(center - Vector2(0, 7), center + Vector2(0, 7), Color(1, 1, 1, 0.6), 1.0)


func _draw_text_element(positions: Dictionary, key: String, sample: String, color: Color,
		surface: Vector2, align_override: int = -1) -> void:
	if not positions.has(key):
		return
	var v = positions[key]
	var p := Vector2.ZERO
	var align := int(HudText.Align.LEFT)
	if v is Vector4i:
		# (x, y, hidden, align) — the original's 4-field positioned-text layout.
		if v.z != 0:
			return # hidden in this hudpos.def
		p = Vector2(v.x, v.y)
		align = v.w
	elif v is Vector2i:
		p = Vector2(v.x, v.y)
	if p == Vector2.ZERO:
		return # element unset in this hudpos.def
	if align_override >= 0:
		align = align_override
	if _font != null:
		HudText.draw_text(self, _font, p, surface, sample, color, align)
	else:
		var sp := HudLayout.scale_point(p, surface)
		_draw_label(sp, sample, color, align)


func _draw_placeholder(anchor_design: Vector2, design_size: Vector2, surface: Vector2, label: String) -> void:
	var r := HudLayout.scale_rect(Rect2(anchor_design, design_size), surface)
	draw_rect(r, Color(0.5, 0.5, 0.55, 0.25), true)
	draw_rect(r, Color(0.6, 0.6, 0.65, 0.6), false, 1.0)
	_draw_label(r.position + Vector2(2, 12), label, Color(0.75, 0.75, 0.8))


func _draw_label(pos: Vector2, text: String, color: Color, align: int = 0) -> void:
	var f := ThemeDB.fallback_font
	var fs := ThemeDB.fallback_font_size
	var draw_pos := pos
	if align == int(HudText.Align.RIGHT):
		draw_pos.x -= f.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
	elif align == int(HudText.Align.CENTER):
		draw_pos.x -= f.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x * 0.5
	draw_string(f, draw_pos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, color)
