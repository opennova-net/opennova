class_name MissionEndScreen
extends Control

## The SP end-of-round cine's device half. Every frame it reads the engine's
## cine (Simulation.get_epilog_cine; engine world/epilog_cine.h owns the
## schedule, the stage machines, each node's live alpha and every value a line
## shows) and draws the live timeline nodes in the engine's two passes: the
## images and fades first, then the text and counter lines, then the cinematic
## bars over everything. Node positions are the cine's 1024 x 768 space scaled
## to the screen; the lines use the large HUD label font at its width/800 slot
## scale. Nothing draws once the engine's cine has stopped. The exits are the
## engine's (the round-over keys and the timeouts store the mission exit the
## shell routes); this node only draws.

var _sim: Simulation = null
var _banner := Callable()
var _root: ResourceRoot = null
var _font: FontFile = null
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
	if root != null:
		var res: FntResource = root.load_font(HudPos.loading_splash_continue_font())
		if res != null:
			_font = res.to_font_file()


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
				CineEventRecord.KIND_EPILOG_COUNTER:
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


func _draw_text_line(e: CineEventRecord) -> void:
	if _font == null:
		return
	var text := ""
	if e.text_source == CineEventRecord.TEXT_SOURCE_BANNER:
		text = String(_banner.call()) if _banner.is_valid() else ""
	else:
		text = Strings.lookup_display(Strings.TABLE_GAMETEXT, e.text_section, e.text_key)
	if text.is_empty():
		return
	var fscale := _font_scale()
	var left := e.x * size.x / HudPos.DESIGN_WIDTH
	var top := e.y * size.y / HudPos.DESIGN_HEIGHT
	var width := e.box_width * size.x / HudPos.DESIGN_WIDTH
	var bottom := top + e.box_height * size.y / HudPos.DESIGN_HEIGHT
	draw_set_transform(Vector2.ZERO, 0.0, Vector2(fscale, fscale))
	HudPos.draw_wrapped_text(self, _font, _font_size(), text, int(left / fscale),
			int(top / fscale), int(width / fscale), int(bottom / fscale),
			HORIZONTAL_ALIGNMENT_CENTER, e.draw_color)
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)


func _draw_counter(e: CineEventRecord) -> void:
	if _font == null:
		return
	var fscale := _font_scale()
	var fs := _font_size()
	var y := e.row_y * size.y / HudPos.DESIGN_HEIGHT / fscale
	var ascent := _font.get_ascent(fs)
	var color := e.draw_color
	draw_set_transform(Vector2.ZERO, 0.0, Vector2(fscale, fscale))
	var label := Strings.lookup_display(Strings.TABLE_GAMETEXT, "Epilog", e.label_key)
	var label_x := e.label_x * size.x / HudPos.DESIGN_WIDTH / fscale
	draw_string(_font, Vector2(label_x, y + ascent), label, HORIZONTAL_ALIGNMENT_LEFT, -1,
			fs, color)
	var value := e.value_text
	if not value.is_empty():
		var value_w := _font.get_string_size(value, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		var value_x := e.value_x * size.x / HudPos.DESIGN_WIDTH / fscale
		draw_string(_font, Vector2(value_x - value_w, y + ascent), value,
				HORIZONTAL_ALIGNMENT_LEFT, -1, fs, color)
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)


func _draw_bars(alpha: float) -> void:
	var bar := float(EpilogCineState.bar_height(int(size.x), int(size.y)))
	if bar <= 0.0:
		return
	var black := Color(0.0, 0.0, 0.0, alpha)
	draw_rect(Rect2(0.0, 0.0, size.x, bar), black)
	draw_rect(Rect2(0.0, size.y - bar, size.x, bar), black)


func _font_scale() -> float:
	return size.x / float(HudPos.SPLASH_FONT_SCALE_BASE_W)


func _font_size() -> int:
	var fs := _font.get_fixed_size() if _font != null else 0
	return fs if fs > 0 else 16


# The cine's full-screen images load through the cine fade loader (a .tga, else
# its .dds; any other name the PCX reader, then the TGA reader); an image that
# does not load draws nothing.
func _image(image_name: String) -> Texture2D:
	if not _images.has(image_name):
		_images[image_name] = _root.load_texture(image_name,
				ResourceRoot.TEXTURE_LOADER_CINE_FADE) if _root != null else null
	return _images[image_name]
