class_name PlayerViewEffects
extends Control

## Retail first-person view effects drawn behind the normal HUD. The original keeps
## the binocular mask/rangefinder and NVG presentation separate from the ordinary
## HUD dispatcher, so health, stance, ammo, objectives, and triggered text remain
## visible while either effect is active. The damage-feedback quads and the sun
## veil are the overlay's own (HudOverlay's view veil, over the main HUD).

# The design space, overlay rects, digit metrics, and NVG modulate are the
# engine's HudPos constants/statics — the witnesses live at the engine home,
# engine/runtime/hud hud/view_effects.h.

var _root: ResourceRoot
var _binocular_mask: Texture2D
var _binocular_crosshair: Texture2D
var _binocular_numbers: Texture2D
var _nvg_mask: Texture2D
var _nvg_scale: Texture2D
# The presenter's per-frame view facts (update_view).
var _binoculars_view_active := false
var _binocular_range := 1
var _nvg_visible := false
var _nvg_gain := 0
var _range_display := 0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	# The underwater murk quad is not a view effect here: it draws in each 3D
	# view's post-particle overlay pass, before the frame effects
	# (SceneOverlayCompositorEffect). The NVG image itself (the 512-square scene,
	# its persistent glow and the green tint composite) is the terminal FrameFx
	# pass's; this Control draws only the NVG.tga mask and the gain scale over it
	# (see _draw_nvg).


func set_resource_root(root: ResourceRoot) -> void:
	_root = root
	# The names are the engine's (HudPos.view_effect_texture), each through its
	# retail loader: the masks through the stage loader, the digits and the NVG
	# scale through the file loader (docs/render/render-material-re.md "Texture
	# loaders").
	_binocular_mask = _load_texture(HudPos.view_effect_texture(HudPos.VIEW_TEXTURE_BINOCULAR_MASK),
			ResourceRoot.TEXTURE_LOADER_STAGE)
	_binocular_crosshair = _load_texture(
			HudPos.view_effect_texture(HudPos.VIEW_TEXTURE_BINOCULAR_CROSSHAIR),
			ResourceRoot.TEXTURE_LOADER_STAGE)
	_binocular_numbers = _load_texture(
			HudPos.view_effect_texture(HudPos.VIEW_TEXTURE_BINOCULAR_DIGITS),
			ResourceRoot.TEXTURE_LOADER_FILE)
	_nvg_mask = _load_texture(HudPos.view_effect_texture(HudPos.VIEW_TEXTURE_NVG_MASK),
			ResourceRoot.TEXTURE_LOADER_STAGE)
	_nvg_scale = _load_texture(HudPos.view_effect_texture(HudPos.VIEW_TEXTURE_NVG_SCALE),
			ResourceRoot.TEXTURE_LOADER_FILE)
	queue_redraw()


func update_view(binoculars_view_active: bool, binocular_range: int,
		nvg_visible: bool, nvg_gain: int) -> void:
	# A redraw re-records this item in Godot's deferred flush every frame; the
	# overlay only changes with its inputs (and the rangefinder easing).
	var changed := binoculars_view_active != _binoculars_view_active \
			or binocular_range != _binocular_range \
			or nvg_visible != _nvg_visible or nvg_gain != _nvg_gain
	_binoculars_view_active = binoculars_view_active
	_binocular_range = binocular_range
	_nvg_visible = nvg_visible
	_nvg_gain = nvg_gain
	if _binoculars_view_active:
		var eased := smooth_range_value(_range_display, _binocular_range)
		changed = changed or eased != _range_display
		_range_display = eased
	if changed:
		queue_redraw()


## Retail's persistent rangefinder easing. Large corrections step quickly while
## the final digits settle one unit at a time; the value is intentionally retained
## while the binocular view is temporarily suppressed or toggled away. The math
## is the engine's HudPos.binocular_range_step — the witness lives at the engine
## home, hud/view_effects.h (engine: runtime/hud/view_effects.h).
static func smooth_range_value(current: int, target: int) -> int:
	return HudPos.binocular_range_step(current, target)


## Whether this frame draws the NVG mask and gain scale (the published
## first-person NVG view).
func is_nvg_mask_visible() -> bool:
	return _nvg_visible


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		queue_redraw()


func _draw() -> void:
	# The view effects are the original's window coordinates, D3D9 pixel
	# centres on the integers (HudPos.d3d9_screen_offset).
	draw_set_transform(HudPos.d3d9_screen_offset())
	var surface := size if size.x > 1.0 and size.y > 1.0 else get_viewport_rect().size
	if _nvg_visible:
		_draw_nvg(surface)
	if _binoculars_view_active:
		_draw_binoculars(surface)


func _draw_nvg(surface: Vector2) -> void:
	if _nvg_mask != null:
		draw_texture_rect(_nvg_mask, Rect2(Vector2.ZERO, surface), false)
	if _nvg_scale == null:
		return
	var gain := clampi(_nvg_gain, 0, 4)
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


func _load_texture(name: String, loader: ResourceRoot.TextureLoader) -> Texture2D:
	return _root.load_texture(name, loader) if _root != null else null
