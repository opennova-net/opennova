class_name RenderCaptureVariant
extends RefCounted

## Typed renderer-capture control record. Diagnostic probes pass this across
## the capture-session seam; dictionaries appear only when the record is
## serialized into a scratch manifest.

var id := ""
var debug_draw := Viewport.DEBUG_DRAW_DISABLED
var dynamic_shadow_enabled := true
var static_terrain_shadow_enabled := true
var suppressed_dynamic_caster_bms_ids := PackedInt32Array()
var suppressed_static_caster_bms_ids := PackedInt32Array()


func _init(
		variant_id: String,
		variant_debug_draw: int,
		dynamic_enabled: bool,
		static_terrain_enabled: bool,
		suppressed_dynamic_bms_ids: PackedInt32Array = PackedInt32Array(),
		suppressed_static_bms_ids: PackedInt32Array = PackedInt32Array(),
		) -> void:
	id = variant_id
	debug_draw = variant_debug_draw
	dynamic_shadow_enabled = dynamic_enabled
	static_terrain_shadow_enabled = static_terrain_enabled
	suppressed_dynamic_caster_bms_ids = suppressed_dynamic_bms_ids.duplicate()
	suppressed_static_caster_bms_ids = suppressed_static_bms_ids.duplicate()


func to_json_value() -> Dictionary:
	return {
		"id": id,
		"debug_draw": debug_draw,
		"dynamic_shadow_enabled": dynamic_shadow_enabled,
		"static_terrain_shadow_enabled": static_terrain_shadow_enabled,
		"suppressed_dynamic_caster_bms_ids": Array(
				suppressed_dynamic_caster_bms_ids),
		"suppressed_static_caster_bms_ids": Array(
				suppressed_static_caster_bms_ids),
	}
