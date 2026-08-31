extends RefCounted

## Capture-variant shadow controls (ADR 0043): the render-fixture probe's
## variant applier over the scene shadow state — the sun's CSM toggle, the
## static terrain-shadow bake enable, the per-BMS static suppression list and
## the viewport debug-draw mode. Replaces the retired render-slot attribution
## session; diagnostics echo the applied variant for the manifest.

const RenderCaptureVariant := preload("res://probes/render/render_capture_variant.gd")

var _world: Node = null
var _viewport: Viewport = null
var _saved_dynamic := true
var _saved_static := true
var _saved_suppressed := PackedInt32Array()
var _saved_debug_draw := Viewport.DEBUG_DRAW_DISABLED
var _applied: RenderCaptureVariant = null


func begin(world: Node, viewport: Viewport) -> Error:
	if world == null or viewport == null:
		return ERR_INVALID_PARAMETER
	_world = world
	_viewport = viewport
	var sun := _sun()
	_saved_dynamic = sun.shadow_enabled if sun != null else true
	var terrain := _terrain()
	if terrain != null:
		_saved_static = terrain.is_static_terrain_shadow_enabled()
		_saved_suppressed = terrain.get_suppressed_static_shadow_bms_ids()
	_saved_debug_draw = viewport.debug_draw
	return OK


func apply_variant(variant: RenderCaptureVariant) -> Error:
	if variant == null or _world == null or _viewport == null:
		return ERR_INVALID_PARAMETER
	var sun := _sun()
	if sun != null:
		sun.shadow_enabled = bool(variant.dynamic_shadow_enabled)
	var terrain := _terrain()
	if terrain != null:
		terrain.set_static_terrain_shadow_enabled(
				bool(variant.static_terrain_shadow_enabled))
		terrain.set_suppressed_static_shadow_bms_ids(
				variant.suppressed_static_caster_bms_ids)
	_viewport.debug_draw = variant.debug_draw
	_applied = variant
	return OK


func get_variant_diagnostics() -> RenderCaptureVariant:
	return _applied


func finish() -> void:
	var sun := _sun()
	if sun != null:
		sun.shadow_enabled = _saved_dynamic
	var terrain := _terrain()
	if terrain != null:
		terrain.set_static_terrain_shadow_enabled(_saved_static)
		terrain.set_suppressed_static_shadow_bms_ids(_saved_suppressed)
	if _viewport != null:
		_viewport.debug_draw = _saved_debug_draw
	_world = null
	_viewport = null
	_applied = null


func _sun() -> SunShadow:
	return _world.get_node_or_null("SunShadow") as SunShadow if _world != null else null


func _terrain() -> Terrain:
	return _world.get_node_or_null("Terrain") as Terrain if _world != null else null
