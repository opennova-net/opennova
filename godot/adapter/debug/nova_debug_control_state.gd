class_name NovaDebugControlState
extends RefCounted
## One authoritative observation of a NovaDebugControlDef.

var id: StringName
var kind := NovaDebugControlDef.Kind.CHECK
var value: Variant
var desired_value: Variant
var available := false
var writable := false
var authoritative := false
var suspended := false
var reason: String


## JSON-facing representation used only at snapshot/transport boundaries.
func to_json_value() -> Variant:
	return {
		"id": String(id),
		"kind": _kind_name(),
		"value": _json_value(value),
		"desired_value": _json_value(desired_value),
		"available": available,
		"writable": writable,
		"authoritative": authoritative,
		"suspended": suspended,
		"reason": reason,
	}


func _kind_name() -> String:
	match kind:
		NovaDebugControlDef.Kind.CHECK:
			return "check"
		NovaDebugControlDef.Kind.SLIDER:
			return "slider"
		NovaDebugControlDef.Kind.ENUM:
			return "enum"
		NovaDebugControlDef.Kind.ACTION:
			return "action"
	return "unknown"


static func _json_value(input: Variant) -> Variant:
	if input is Vector2:
		return {"x": input.x, "y": input.y}
	if input is Vector3:
		return {"x": input.x, "y": input.y, "z": input.z}
	if input is Color:
		return {"r": input.r, "g": input.g, "b": input.b, "a": input.a}
	if input is StringName:
		return String(input)
	if input is Dictionary:
		var mapped := {}
		for key in input:
			mapped[String(key)] = _json_value(input[key])
		return mapped
	if input is Array:
		var mapped: Array = []
		for value in input:
			mapped.append(_json_value(value))
		return mapped
	if input is PackedStringArray:
		return Array(input)
	if input is PackedInt32Array or input is PackedInt64Array \
			or input is PackedFloat32Array or input is PackedFloat64Array:
		return Array(input)
	if input is Object:
		return null
	return input
