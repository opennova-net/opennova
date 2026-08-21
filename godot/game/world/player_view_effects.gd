class_name PlayerViewEffects
extends Control

## Retail first-person view effects drawn behind the normal HUD. The original keeps
## the binocular mask/rangefinder and NVG presentation separate from the ordinary
## HUD dispatcher, so health, stance, ammo, objectives, and triggered text remain
## visible while either effect is active.

# The design space, overlay rects, digit metrics, and NVG modulate are the
# engine's HudPos constants/statics — the witnesses live at the engine home,
# engine/runtime/hud hud/view_effects.h.
const NVG_SHADER := preload("res://shaders/nvg_view.gdshader")
const SUN_VEIL_SHADER := preload("res://shaders/sun_veil_overlay.gdshader")

var _root: ResourceRoot
var _binocular_mask: Texture2D
var _binocular_crosshair: Texture2D
var _binocular_numbers: Texture2D
var _nvg_mask: Texture2D
var _nvg_scale: Texture2D
var _underwater_murk: ColorRect
var _sun_veil: ColorRect
var _nvg_post: ColorRect
var _environment: MissionEnvironment
var _environment_light_state: EnvLightState
var _info: Dictionary = {}
var _range_display := 0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	# Retail draws this standard source-over viewport quad after the complete
	# world + first-person weapon and before every HUD overlay. This Control is
	# mounted on HUD CanvasLayer 1 behind its parent, so it follows ViewmodelPass
	# layer 0 and precedes the parent's normal HUD draw list.
	_underwater_murk = ColorRect.new()
	_underwater_murk.name = "UnderwaterMurk"
	_underwater_murk.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_underwater_murk.show_behind_parent = true
	add_child(_underwater_murk, false, Node.INTERNAL_MODE_BACK)
	_underwater_murk.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_underwater_murk.visible = false
	# The sun-glare screen veil: a fullscreen white quad whose alpha is the
	# dot^32 glare byte, drawn over the complete scene [orig:
	# Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0 from
	# Render_ProcessMainSceneFrame @ 0x5cac4b]. Celestial pushes the
	# opennova_sun_veil_alpha shader global every advanced frame, so the rect
	# needs no per-frame script drive and stays correct in frozen captures.
	_sun_veil = ColorRect.new()
	_sun_veil.name = "SunVeil"
	_sun_veil.color = Color.WHITE
	_sun_veil.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_sun_veil.show_behind_parent = true
	var veil_material := ShaderMaterial.new()
	veil_material.shader = SUN_VEIL_SHADER
	_sun_veil.material = veil_material
	add_child(_sun_veil, false, Node.INTERNAL_MODE_BACK)
	_sun_veil.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
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
	_sync_underwater_murk()


func set_environment(environment: MissionEnvironment) -> void:
	var callback := Callable(self, "_sync_underwater_murk")
	if _environment != null and is_instance_valid(_environment) \
			and _environment.underwater_overlay_changed.is_connected(callback):
		_environment.underwater_overlay_changed.disconnect(callback)
	if _environment_light_state != null \
			and _environment_light_state.changed.is_connected(callback):
		_environment_light_state.changed.disconnect(callback)
	_environment = environment
	_environment_light_state = _environment.get_light_state() \
			if _environment != null else null
	if _environment != null:
		_environment.underwater_overlay_changed.connect(callback)
	if _environment_light_state != null:
		# TOD/weather can change Env_WaterColorLit without crossing the plane.
		_environment_light_state.changed.connect(callback)
	_sync_underwater_murk()


func _sync_underwater_murk() -> void:
	if _underwater_murk == null:
		return
	if _environment == null or not is_instance_valid(_environment):
		_underwater_murk.visible = false
		return
	# Above water the murk is invisible; skip the native fog rebuild that
	# get_underwater_overlay_color() performs on every env-generation bump.
	if not _environment.is_underwater_overlay_view():
		_underwater_murk.visible = false
		return
	var lit := _environment.get_underwater_overlay_color()
	var alpha := float(_environment.get_underwater_overlay_alpha_byte()) / 255.0
	_underwater_murk.color = Color(lit.x, lit.y, lit.z, alpha)
	_underwater_murk.visible = true


func set_resource_root(root: ResourceRoot) -> void:
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
## while the binocular view is temporarily suppressed or toggled away. The math
## is the engine's HudPos.binocular_range_step — the witness lives at the engine
## home, hud/view_effects.h [orig: the misnamed HUD_DrawSpeedometer @0x590810].
static func smooth_range_value(current: int, target: int) -> int:
	return HudPos.binocular_range_step(current, target)


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
	var source := Rect2(0.0, float(gain * HudPos.VIEW_DIGIT_CELL),
			float(HudPos.VIEW_DIGIT_CELL), float(HudPos.VIEW_DIGIT_CELL))
	draw_texture_rect_region(_nvg_scale,
			HudPos.scale_rect(HudPos.nvg_scale_rect(), surface), source,
			HudPos.nvg_scale_modulate())


func _draw_binoculars(surface: Vector2) -> void:
	if _binocular_mask != null:
		draw_texture_rect(_binocular_mask, Rect2(Vector2.ZERO, surface), false)
	if _binocular_crosshair != null:
		draw_texture_rect(_binocular_crosshair,
				HudPos.scale_rect(HudPos.binocular_crosshair_rect(), surface), false)
	if _binocular_numbers == null:
		return
	var digits := "%04d" % clampi(_range_display, 0, 1000)
	for i in 4:
		var digit := digits.unicode_at(i) - 48
		var target := Rect2(
				HudPos.binocular_digit_pos()
					+ Vector2(HudPos.BINOCULAR_DIGIT_STEP * i, 0.0),
				Vector2(HudPos.VIEW_DIGIT_CELL, HudPos.VIEW_DIGIT_CELL))
		var source := Rect2(0.0, float(digit * HudPos.VIEW_DIGIT_CELL),
				float(HudPos.VIEW_DIGIT_CELL), float(HudPos.VIEW_DIGIT_CELL))
		draw_texture_rect_region(_binocular_numbers,
				HudPos.scale_rect(target, surface), source)


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
