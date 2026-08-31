extends RefCounted

## Capture-variant shadow controls (ADR 0043): the render-fixture probe's
## variant applier over the scene shadow state — the sun's CSM toggle, the
## static-caster population toggle (the SHADOWS_ONLY twins / marked models the
## CSM consumes) and the viewport debug-draw mode. Replaces the retired
## render-slot attribution session and the CPU bake's enable/suppression
## seams; diagnostics echo the applied variant for the manifest.

const RenderCaptureVariant := preload("res://probes/render/render_capture_variant.gd")

var _world: GameWorld = null
var _viewport: Viewport = null
var _saved_dynamic := true
var _saved_debug_draw := Viewport.DEBUG_DRAW_DISABLED
# instance_id -> original cast_shadows_setting for the static casters the
# current variant disabled.
var _static_cast_saves: Dictionary = {}
var _applied: RenderCaptureVariant = null


func begin(world: GameWorld, viewport: Viewport) -> Error:
	if world == null or viewport == null:
		return ERR_INVALID_PARAMETER
	_world = world
	_viewport = viewport
	var sun := _sun()
	_saved_dynamic = sun.shadow_enabled if sun != null else true
	_saved_debug_draw = viewport.debug_draw
	_static_cast_saves.clear()
	return OK


func apply_variant(variant: RenderCaptureVariant) -> Error:
	if variant == null or _world == null or _viewport == null:
		return ERR_INVALID_PARAMETER
	var sun := _sun()
	if sun != null:
		sun.shadow_enabled = bool(variant.dynamic_shadow_enabled)
	_set_static_casting(bool(variant.static_terrain_shadow_enabled))
	_viewport.debug_draw = variant.debug_draw
	_applied = variant
	return OK


func get_variant_diagnostics() -> RenderCaptureVariant:
	return _applied


func finish() -> void:
	var sun := _sun()
	if sun != null:
		sun.shadow_enabled = _saved_dynamic
	_set_static_casting(true)
	if _viewport != null:
		_viewport.debug_draw = _saved_debug_draw
	_world = null
	_viewport = null
	_applied = null


func _sun() -> SunShadow:
	if _world == null or not is_instance_valid(_world):
		return null
	return _world.get_sun_shadow_node() as SunShadow


func _set_static_casting(enabled: bool) -> void:
	if enabled:
		for id_v in _static_cast_saves.keys():
			var instance := instance_from_id(int(id_v)) as GeometryInstance3D
			if instance != null and is_instance_valid(instance):
				instance.cast_shadow = int(_static_cast_saves[id_v])
		_static_cast_saves.clear()
		return
	_disable_static_casters(_world)


func _disable_static_casters(root: Node) -> void:
	if root == null:
		return
	if root is GeometryInstance3D:
		var instance := root as GeometryInstance3D
		if (instance.layers & Water.VISUAL_LAYER_STATIC_SHADOW_CASTER) != 0 \
				and instance.cast_shadow != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF:
			if not _static_cast_saves.has(instance.get_instance_id()):
				_static_cast_saves[instance.get_instance_id()] = instance.cast_shadow
			instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	for child in root.get_children():
		_disable_static_casters(child)
