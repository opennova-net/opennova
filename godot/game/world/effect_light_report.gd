class_name EffectLightReport
extends RefCounted

## Typed diagnostic snapshot for EffectWorld's dynamic point-light pool.
## LightScene emits a Dictionary across the native GDExtension FFI; decode it
## once here so every GDScript consumer sees a checked contract. The inverse
## conversion is reserved for the renderer-diagnostics JSON boundary.


class Row:
	extends RefCounted

	var position := Vector3.ZERO
	var color := Color.WHITE
	var range := 0.0
	var attenuation_quadratic := 0.0
	var handle := 0
	var retail_handle := 0


	static func from_ffi_dictionary(value: Dictionary) -> Row:
		var row := Row.new()
		row.position = value.get("position", Vector3.ZERO)
		row.color = value.get("color", Color.WHITE)
		row.range = float(value.get("range", 0.0))
		row.attenuation_quadratic = float(value.get("atten2", 0.0))
		row.handle = int(value.get("handle", 0))
		row.retail_handle = int(value.get("retail_handle", 0))
		return row


	func to_json_value() -> Dictionary:
		return {
			"position": position,
			"color": color,
			"range": range,
			"atten2": attenuation_quadratic,
			"handle": handle,
			"retail_handle": retail_handle,
		}


var live := 0
var high_water := 0
var last_query := 0
var selected := 0
var selection_mode := ""
var owner_isolation := ""
var scene_lights := 0
var rows: Array[Row] = []


static func from_ffi_dictionary(value: Dictionary) -> EffectLightReport:
	var report := EffectLightReport.new()
	report.live = int(value.get("live", 0))
	report.high_water = int(value.get("high_water", 0))
	report.last_query = int(value.get("last_query", 0))
	report.selected = int(value.get("selected", 0))
	report.selection_mode = String(value.get("selection_mode", ""))
	report.owner_isolation = String(value.get("owner_isolation", ""))
	report.scene_lights = int(value.get("scene_lights", 0))
	for row_value in value.get("rows", []):
		if row_value is Dictionary:
			report.rows.append(Row.from_ffi_dictionary(row_value))
	return report


func to_json_value() -> Dictionary:
	var encoded_rows: Array = []
	for row in rows:
		encoded_rows.append(row.to_json_value())
	return {
		"live": live,
		"high_water": high_water,
		"last_query": last_query,
		"selected": selected,
		"selection_mode": selection_mode,
		"owner_isolation": owner_isolation,
		"scene_lights": scene_lights,
		"rows": encoded_rows,
	}
