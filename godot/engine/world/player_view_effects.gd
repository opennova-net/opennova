class_name PlayerViewEffects
extends Control

## Retail first-person view effects drawn behind the normal HUD. The original keeps
## the binocular mask/rangefinder and NVG presentation separate from the ordinary
## HUD dispatcher, so health, stance, ammo, objectives, and triggered text remain
## visible while either effect is active.

const DESIGN_SIZE := Vector2(1024.0, 768.0)
const BINOCULAR_CROSSHAIR_RECT := Rect2(384.0, 256.0, 256.0, 256.0)
const BINOCULAR_DIGIT_POS := Vector2(486.0, 683.0)
const BINOCULAR_DIGIT_STEP := 10.0
const NVG_SCALE_RECT := Rect2(960.0, 32.0, 48.0, 32.0)
const DIGIT_SIZE := Vector2(16.0, 16.0)
const NVG_SCALE_MODULATE := Color(127.0 / 255.0, 127.0 / 255.0, 127.0 / 255.0, 1.0)
const NVG_SHADER := preload("res://shaders/nvg_view.gdshader")

var _root: NovaResourceRoot
var _binocular_mask: Texture2D
var _binocular_crosshair: Texture2D
var _binocular_numbers: Texture2D
var _nvg_mask: Texture2D
var _nvg_scale: Texture2D
var _nvg_post: ColorRect
var _info: Dictionary = {}
var _range_display := 0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_nvg_post = ColorRect.new()
	_nvg_post.name = "NvgPost"
	_nvg_post.color = Color.WHITE
	_nvg_post.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_nvg_post.show_behind_parent = true
	var shader_material := ShaderMaterial.new()
	shader_material.shader = NVG_SHADER
	_nvg_post.material = shader_material
	add_child(_nvg_post, false, Node.INTERNAL_MODE_BACK)
	_nvg_post.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_nvg_post.visible = bool(_info.get("nvg_visible", false))


func set_resource_root(root: NovaResourceRoot) -> void:
	_root = root
	_binocular_mask = _load_texture("Binoculr.tga")
	_binocular_crosshair = _load_texture("BinoCH.tga")
	_binocular_numbers = _load_texture("BNumbers.tga")
	_nvg_mask = _load_texture("NVG.tga")
	_nvg_scale = _load_texture("Nvgscale.tga")
	queue_redraw()


func update_info(info: Dictionary) -> void:
	_info = info
	if bool(_info.get("binoculars_view_active", false)):
		_range_display = smooth_range_value(
				_range_display, int(_info.get("binocular_range", 1)))
	if _nvg_post != null:
		_nvg_post.visible = bool(_info.get("nvg_visible", false))
	queue_redraw()


## Retail's persistent rangefinder easing. Large corrections step quickly while
## the final digits settle one unit at a time; the value is intentionally retained
## while the binocular view is temporarily suppressed or toggled away.
## [orig: the misnamed HUD_DrawSpeedometer @0x590810]
static func smooth_range_value(current: int, target: int) -> int:
	target = clampi(target, 1, 1000)
	var delta := target - current
	var magnitude := absi(delta)
	if magnitude == 0:
		return current
	if magnitude > 1000:
		return target
	var step := 1
	if magnitude > 111:
		step = 111
	elif magnitude > 33:
		step = 33
	elif magnitude > 11:
		step = 11
	elif magnitude > 3:
		step = 3
	return current + step * (1 if delta > 0 else -1)


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		queue_redraw()


func _draw() -> void:
	var surface := size if size.x > 1.0 and size.y > 1.0 else get_viewport_rect().size
	if bool(_info.get("nvg_visible", false)):
		_draw_nvg(surface)
	if bool(_info.get("binoculars_view_active", false)):
		_draw_binoculars(surface)


func _draw_nvg(surface: Vector2) -> void:
	if _nvg_mask != null:
		draw_texture_rect(_nvg_mask, Rect2(Vector2.ZERO, surface), false)
	if _nvg_scale == null:
		return
	var gain := clampi(int(_info.get("nvg_gain", 0)), 0, 4)
	var source := Rect2(0.0, float(gain * 16), 16.0, 16.0)
	draw_texture_rect_region(_nvg_scale, _scale_rect(NVG_SCALE_RECT, surface), source,
			NVG_SCALE_MODULATE)


func _draw_binoculars(surface: Vector2) -> void:
	if _binocular_mask != null:
		draw_texture_rect(_binocular_mask, Rect2(Vector2.ZERO, surface), false)
	if _binocular_crosshair != null:
		draw_texture_rect(_binocular_crosshair,
				_scale_rect(BINOCULAR_CROSSHAIR_RECT, surface), false)
	if _binocular_numbers == null:
		return
	var digits := "%04d" % clampi(_range_display, 0, 1000)
	for i in 4:
		var digit := digits.unicode_at(i) - 48
		var target := Rect2(
				BINOCULAR_DIGIT_POS + Vector2(BINOCULAR_DIGIT_STEP * i, 0.0),
				DIGIT_SIZE)
		var source := Rect2(0.0, float(digit * 16), 16.0, 16.0)
		draw_texture_rect_region(_binocular_numbers, _scale_rect(target, surface), source)


func _scale_rect(design_rect: Rect2, surface: Vector2) -> Rect2:
	var scale_v := surface / DESIGN_SIZE
	return Rect2(design_rect.position * scale_v, design_rect.size * scale_v)


func _load_texture(name: String) -> Texture2D:
	if _root == null or name.is_empty():
		return null
	var bytes := _root.read_file(name.get_file())
	if bytes.is_empty():
		return null
	var image := Image.new()
	if image.load_tga_from_buffer(bytes) != OK:
		return null
	return ImageTexture.create_from_image(image)
