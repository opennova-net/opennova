class_name PlayerViewEffects
extends Control

## Retail first-person view effects drawn behind the normal HUD. The original keeps
## the binocular mask/rangefinder and NVG presentation separate from the ordinary
## HUD dispatcher, so health, stance, ammo, objectives, and triggered text remain
## visible while either effect is active.

# The design space, overlay rects, digit metrics, and NVG modulate are the
# engine's HudPos constants/statics — the witnesses live at the engine home,
# engine/runtime/hud hud/view_effects.h.
const SUN_VEIL_SHADER := preload("res://shaders/sun_veil_overlay.gdshader")

var _root: ResourceRoot
var _binocular_mask: Texture2D
var _binocular_crosshair: Texture2D
var _binocular_numbers: Texture2D
var _nvg_mask: Texture2D
var _nvg_scale: Texture2D
var _vignette: Texture2D
var _underwater_murk: ColorRect
var _sun_veil: ColorRect
# The three fullscreen damage-feedback quads the retail scene frame draws last
# (see update_damage_feedback).
var _white_flash: ColorRect
var _red_vignette: TextureRect
var _revive_tint: ColorRect
var _environment: MissionEnvironment
var _environment_light_state: EnvLightState
# The presenter's per-frame view facts (update_view).
var _binoculars_view_active := false
var _binocular_range := 1
var _nvg_visible := false
var _nvg_gain := 0
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
	# The NVG image itself (the 512-square scene, its persistent glow and the
	# green tint composite) is the terminal FrameFx pass's; this Control draws
	# only the NVG.tga mask and the gain scale over it (see _draw_nvg).
	_build_damage_feedback_quads()
	_sync_underwater_murk()


## The three fullscreen damage-feedback quads, in the order the retail scene
## frame emits them: the white hit flash, the red damage vignette, then the
## medic revive tint. They are ordinary (non behind-parent) children, so they
## follow this Control's own binocular/NVG draw, matching retail's placement at
## the very end of the frame. Retail's own order also puts them AFTER the HUD
## overlay pass and BEFORE the sun veil; our HUD is this node's parent and the
## sun veil is a behind-parent sibling, so both of those neighbours sit on the
## other side of the three quads here. That stacking difference is recorded in
## docs/interface/hud-re.md.
func _build_damage_feedback_quads() -> void:
	# 1. The white hit flash: an untextured white quad whose alpha IS the word.
	# retail: quad colour (word << 24) | 0xFFFFFF through the untextured
	# iterated-colour material (the same one the sun-glare veil uses).
	_white_flash = ColorRect.new()
	_white_flash.name = "ScreenFlashWhite"
	_white_flash.color = Color(1.0, 1.0, 1.0, 0.0)
	_white_flash.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_white_flash.visible = false
	add_child(_white_flash, false, Node.INTERNAL_MODE_BACK)
	_white_flash.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	# 2. The red damage vignette: vignette.tga stretched over the viewport, its
	# TEXTURE alpha multiplied by the vertex alpha and tinted by the vertex
	# colour (retail material flags 593 = AFUNC_BLEND | ASRC_TEXTURExITERATED |
	# COLOR_ITERATED), i.e. Godot's ordinary modulate over an alpha-blended
	# TextureRect. The alpha is capped at 192 and suppressed in camera mode 3;
	# the engine applies both before this feed.
	_red_vignette = TextureRect.new()
	_red_vignette.name = "ScreenFlashVignette"
	_red_vignette.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_red_vignette.stretch_mode = TextureRect.STRETCH_SCALE
	_red_vignette.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_red_vignette.modulate = Color(1.0, 0.0, 0.0, 0.0)
	_red_vignette.visible = false
	add_child(_red_vignette, false, Node.INTERNAL_MODE_BACK)
	_red_vignette.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	# 3. The medic revive tint: retail sends an OPAQUE (alpha 255) quad whose
	# red and green channels fall to 255 - (word >> 1) while blue stays 255, and
	# the word floors at 196 and holds there until the round clears. An opaque
	# source-over quad would wall the view off permanently, so the material
	# behind its quad mode is a MULTIPLY: the frame is tinted blue, deepening
	# with the word. The blend is the one part of the three quads not witnessed
	# byte-for-byte (its render-state slot has no other user); the hold-at-196
	# behaviour is what rules source-over out.
	_revive_tint = ColorRect.new()
	_revive_tint.name = "ScreenFlashReviveTint"
	_revive_tint.color = Color.WHITE
	_revive_tint.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var tint_material := CanvasItemMaterial.new()
	tint_material.blend_mode = CanvasItemMaterial.BLEND_MODE_MUL
	_revive_tint.material = tint_material
	_revive_tint.visible = false
	add_child(_revive_tint, false, Node.INTERNAL_MODE_BACK)
	_revive_tint.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


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
	# The red damage vignette's texture; retail loads it once into the material
	# behind the quad's mode-3 pass.
	_vignette = _load_texture("vignette.tga")
	if _red_vignette != null:
		_red_vignette.texture = _vignette
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


## The three fullscreen damage-feedback quads, fed straight from the engine's
## per-frame view state. The engine owns every word, decay, cap and gate (the
## witnesses live at engine/runtime/world/player_view.h); this only sizes rects
## and picks colours.
##   `white_alpha`   the raw hit-flash word, 0..255, drawn as white at that alpha
##   `red_alpha`     the vignette's DRAW alpha, already capped at 192 and already
##                   zeroed in the free/spectator camera mode
##   `revive`        the revive word, non-zero = draw
##   `revive_channel` 255 - (revive >> 1): the tint's red/green byte, blue is 255
func update_damage_feedback(white_alpha: int, red_alpha: int, revive: int,
		revive_channel: int) -> void:
	if _white_flash != null:
		var show_white := white_alpha > 0
		if show_white:
			_white_flash.color = Color(1.0, 1.0, 1.0,
					clampf(float(white_alpha) / 255.0, 0.0, 1.0))
		_white_flash.visible = show_white
	if _red_vignette != null:
		var show_red := red_alpha > 0 and _vignette != null
		if show_red:
			_red_vignette.modulate = Color(1.0, 0.0, 0.0,
					clampf(float(red_alpha) / 255.0, 0.0, 1.0))
		_red_vignette.visible = show_red
	if _revive_tint != null:
		var show_revive := revive > 0
		if show_revive:
			var channel := clampf(float(revive_channel) / 255.0, 0.0, 1.0)
			_revive_tint.color = Color(channel, channel, 1.0, 1.0)
		_revive_tint.visible = show_revive


## Retail's persistent rangefinder easing. Large corrections step quickly while
## the final digits settle one unit at a time; the value is intentionally retained
## while the binocular view is temporarily suppressed or toggled away. The math
## is the engine's HudPos.binocular_range_step — the witness lives at the engine
## home, hud/view_effects.h [orig: the misnamed HUD_DrawSpeedometer @0x590810].
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


func _load_texture(name: String) -> Texture2D:
	return TgaTexture.load_from_root(_root, name)
