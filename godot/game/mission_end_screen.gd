class_name MissionEndScreen
extends Control

## The SP end-of-round cine's device half. Every frame it reads the engine's
## cine (Simulation.get_epilog_cine; engine world/epilog_cine.h owns the
## schedule, the stage machines, each node's live alpha and every value a line
## shows) and draws the live timeline nodes in the engine's two passes: the
## images and fades first, then the text and counter lines, then the cinematic
## bars over everything. The text and counter lines are the engine's glyph
## runs in each node's HUD label font (HudPos.draw_cine_text and
## HudPos.cine_font_file; engine hud/cine_text.h owns their slot, text,
## placement, wrap and colours), drawn through the font page's material. Nothing draws once the engine's cine has stopped. The
## exits are the engine's (the round-over keys and the timeouts store the
## mission exit the shell routes); this node only draws.

var _sim: Simulation = null
var _banner := Callable()
var _root: ResourceRoot = null
# font file -> [FntResource, its pages as textures (HudPos.font_page_textures)]
var _fonts := {}
var _images := {} # image name -> Texture2D (null when it does not load)
var _cine: EpilogCineState = null


## `banner` returns the stored end-of-round banner the lose screen's banner
## line draws (an empty one draws nothing).
func setup(sim: Simulation, banner: Callable, root: ResourceRoot) -> void:
	_sim = sim
	_banner = banner
	_root = root
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	# The one process-wide glyph program (HudPos.glyph_shader) the font page's
	# material runs over the runs.
	var glyph_material := ShaderMaterial.new()
	glyph_material.shader = HudPos.glyph_shader()
	material = glyph_material


## The cine the screen last drew from (null before the first frame).
func get_cine() -> EpilogCineState:
	return _cine


func _process(_delta: float) -> void:
	_cine = _sim.get_epilog_cine() if _sim != null else null
	queue_redraw()


func _draw() -> void:
	if _cine == null or not _cine.active:
		return
	var frame := _cine.frame
	for second_pass in [false, true]:
		for e: CineEventRecord in _cine.events:
			if e.second_pass != second_pass or not e.is_live_at(frame):
				continue
			match e.kind:
				CineEventRecord.KIND_IMAGE_FADE:
					_draw_image_fade(e)
				CineEventRecord.KIND_TEXT_FADE:
					_draw_text_line(e)
				CineEventRecord.KIND_EPILOG_COUNTER, CineEventRecord.KIND_EPILOG_COUNTER_FONT:
					_draw_counter(e)
	if _cine.bars_fading or _cine.bars_held:
		_draw_bars(_cine.bars_alpha if _cine.bars_fading else 1.0)


func _draw_image_fade(e: CineEventRecord) -> void:
	var color := e.draw_color
	if color.a <= 0.0:
		return
	var full := Rect2(Vector2.ZERO, size)
	if e.fade_source != CineEventRecord.FADE_SOURCE_IMAGE:
		draw_rect(full, color)
		return
	var tex := _image(e.image)
	if tex != null:
		draw_texture_rect(tex, full, false, color)


# The line's gametext string (none for a node that names no key) and the stored
# banner; the engine picks the line by the node's source.
func _draw_text_line(e: CineEventRecord) -> void:
	var game_text := ""
	if not e.text_key.is_empty():
		game_text = Strings.lookup_display(Strings.TABLE_GAMETEXT, e.text_section, e.text_key)
	var banner := String(_banner.call()) if _banner.is_valid() else ""
	var font := _font_for(e)
	HudPos.draw_cine_text(self, font[0], font[1], e, game_text, banner, _screen_size())


func _draw_counter(e: CineEventRecord) -> void:
	var label := Strings.lookup_display(Strings.TABLE_GAMETEXT, "Epilog", e.label_key)
	var font := _font_for(e)
	HudPos.draw_cine_text(self, font[0], font[1], e, label, "", _screen_size())


# The node's label slot's font and pages, loaded once a file; a slot with no file,
# or no root, draws nothing.
func _font_for(e: CineEventRecord) -> Array:
	var file := HudPos.cine_font_file(e, _screen_size().x)
	if not _fonts.has(file):
		var font: FntResource = null
		if _root != null and not file.is_empty():
			font = _root.load_font(file) as FntResource
		_fonts[file] = [font, HudPos.font_page_textures(font)]
	return _fonts[file]


func _draw_bars(alpha: float) -> void:
	var bar := float(EpilogCineState.bar_height(int(size.x), int(size.y)))
	if bar <= 0.0:
		return
	var black := Color(0.0, 0.0, 0.0, alpha)
	draw_rect(Rect2(0.0, 0.0, size.x, bar), black)
	draw_rect(Rect2(0.0, size.y - bar, size.x, bar), black)


func _screen_size() -> Vector2i:
	return Vector2i(roundi(size.x), roundi(size.y))


# The cine's full-screen images load through the cine fade loader (a .tga, else
# its .dds; any other name the PCX reader, then the TGA reader); an image that
# does not load draws nothing.
func _image(image_name: String) -> Texture2D:
	if not _images.has(image_name):
		_images[image_name] = _root.load_texture(image_name,
				ResourceRoot.TEXTURE_LOADER_CINE_FADE) if _root != null else null
	return _images[image_name]
