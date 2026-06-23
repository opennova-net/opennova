class_name GameHud
extends Control

## The runtime in-game HUD overlay. A full-rect Control on the gameplay HUD layer
## (a child of the $HUD CanvasLayer, so the shell's _set_hud_visible hides it behind
## menus). It draws the witnessed HUD elements from a hudpos.def layout (NovaHudPos)
## plus a per-frame "info" dict the host rebuilds each frame — mirroring the original,
## which rebuilds its per-frame HUD info struct every frame and then dispatches the
## element draws. [orig: HUD_BuildEntityInfo @0x4b8440 -> HUD_RenderOverlays @0x5a7bb0]
##
## First cut binds only to already-available player state: health, stance, team,
## objective text, plus the static HUD frame and a center reticle. The weapon-coupled
## elements (ammo, weapon name, clip, the dynamic crosshair spread) and the heading
## radar are deferred (see docs/interface/hud-re.md).

var _hudpos: NovaHudPos
var _root: NovaResourceRoot
var _font: FontFile
var _frame_tex: Texture2D
var _stance_textures: Array[Texture2D] = []
var _positions: Dictionary = {}

# Per-frame state, rebuilt by the host. Defaults keep the HUD sane before the first update.
var _info: Dictionary = {
	"health_fraction": 1.0,
	"stance": 0,
	"team": 0,
	"objective": "",
}


func set_layout(hudpos: NovaHudPos, root: NovaResourceRoot) -> void:
	_hudpos = hudpos
	_root = root
	_font = null
	_frame_tex = null
	_stance_textures.clear()
	_positions = {}
	_load_assets()
	queue_redraw()


func update_info(info: Dictionary) -> void:
	_info = info
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
	_positions = _hudpos.to_dictionary().get("positions", {})

	var frames := _hudpos.get_static_frames()
	if frames.size() > 0:
		_frame_tex = _load_texture(String((frames[0] as Dictionary).get("texture", "")))
	for s in _hudpos.get_stances():
		_stance_textures.append(_load_texture(String((s as Dictionary).get("texture", ""))))


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
	if _hudpos == null or not _hudpos.is_loaded():
		return
	# This overlay is a Control parented directly to a CanvasLayer, which does not drive a child
	# Control's layout — so our own `size` can stay (0,0) and every scaled element would collapse.
	# Draw against the viewport rect, which is the real screen the original scales its HUD to.
	# [orig: overlayCtx @0x24c1420 screen_w/h -> Viewport_ScaleToVirtualCoords @0x5d2b20]
	var surface := get_viewport_rect().size
	var colors := _hudpos.get_colors()

	# Static HUD frame background.
	if _frame_tex != null:
		var frames := _hudpos.get_static_frames()
		if frames.size() > 0:
			var fpos: Vector2i = (frames[0] as Dictionary).get("pos", Vector2i.ZERO)
			draw_texture_rect(_frame_tex, HudLayout.scale_rect(Rect2(fpos, _frame_tex.get_size()), surface), false)

	# Health bar.
	var border: Color = colors.get("health_border", Color(0.35, 0.78, 0.78, 0.78))
	var good: Color = colors.get("tagcolor_good", Color(0.02, 0.98, 0.05))
	var mid: Color = colors.get("tagcolor_middle", Color(0.98, 0.65, 0.03))
	var bad: Color = colors.get("tagcolor_bad", Color(0.69, 0.04, 0.04))
	HudHealthBar.draw(self, Rect2(_hudpos.get_health_rect()),
		float(_info.get("health_fraction", 1.0)), border, good, mid, bad, surface)

	# Stance indicator (the witnessed widget at HUDSTANCEPOS).
	var stance: int = int(_info.get("stance", 0))
	var anchor := Vector2(_hudpos.get_stance_pos())
	if anchor != Vector2.ZERO and stance >= 0 and stance < _stance_textures.size() and _stance_textures[stance] != null:
		HudStance.draw(self, _stance_textures[stance], anchor, surface)

	# Objective / subtitle text (mission effects).
	var objective := String(_info.get("objective", ""))
	if not objective.is_empty():
		var oc: Color = colors.get("hud_textcolor", Color(0.98, 0.84, 0.02))
		var gp := _pos2_of("game_info", Vector2(512, 40))
		if _font != null:
			HudText.draw_text(self, _font, gp, surface, objective, oc, HudText.Align.LEFT)

	# Static center reticle. The weapon crosshair (texture + spread) is deferred with the
	# weapon model; until then draw a minimal center cross to mark aim. [orig: HUD_DrawCrosshair @0x592640]
	var center := surface * 0.5
	var rc := Color(1, 1, 1, 0.7)
	draw_line(center - Vector2(8, 0), center + Vector2(8, 0), rc, 1.0)
	draw_line(center - Vector2(0, 8), center + Vector2(0, 8), rc, 1.0)


# Read a cached element position (Vector3i [x,y,align] or Vector2i) as a design-space Vector2.
func _pos2_of(key: String, fallback: Vector2) -> Vector2:
	if not _positions.has(key):
		return fallback
	var v = _positions[key]
	if v is Vector3i:
		return Vector2(v.x, v.y) if Vector2(v.x, v.y) != Vector2.ZERO else fallback
	if v is Vector2i:
		return Vector2(v.x, v.y) if v != Vector2i.ZERO else fallback
	return fallback
