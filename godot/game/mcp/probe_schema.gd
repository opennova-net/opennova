class_name ProbeSchema
extends RefCounted

## The JSON Schema subset a ProbeDef's input_schema may use, validated on the
## game side before a probe starts: object properties with type (string,
## integer, number, boolean, array, object), enum, minimum/maximum,
## minLength/maxLength, minItems/maxItems, items.type, required, and default
## injection. Unknown keys are rejected (a typo never silently becomes the
## default) and integral floats coerce to int (JSON has no integer type).


static func validate(schema: Dictionary, raw: Variant) -> ProbeSchemaResult:
	var result := ProbeSchemaResult.new()
	if raw == null:
		raw = {}
	if not (raw is Dictionary):
		result.errors.append("args must be a JSON object")
		return result
	var given: Dictionary = raw
	var properties: Dictionary = schema.get("properties", {}) \
			if schema.get("properties") is Dictionary else {}
	var required: Array = schema.get("required", []) \
			if schema.get("required") is Array else []
	for key in given:
		if not properties.has(key):
			result.errors.append("unknown argument '%s'" % key)
	for key in properties:
		var spec: Dictionary = properties[key] if properties[key] is Dictionary else {}
		if given.has(key):
			result.values[key] = _coerce(String(key), spec, given[key], result.errors)
		elif spec.has("default"):
			var fallback: Variant = spec["default"]
			result.values[key] = fallback.duplicate(true) \
					if fallback is Dictionary or fallback is Array else fallback
		elif key in required:
			result.errors.append("missing required argument '%s'" % key)
	result.ok = result.errors.is_empty()
	return result


static func _coerce(key: String, spec: Dictionary, value: Variant,
		errors: PackedStringArray) -> Variant:
	var type := String(spec.get("type", ""))
	match type:
		"string":
			if typeof(value) != TYPE_STRING:
				errors.append("'%s' must be a string" % key)
				return value
			var text := String(value)
			if spec.has("minLength") and text.length() < int(spec["minLength"]):
				errors.append("'%s' must be at least %d characters" % [key, int(spec["minLength"])])
			if spec.has("maxLength") and text.length() > int(spec["maxLength"]):
				errors.append("'%s' must be at most %d characters" % [key, int(spec["maxLength"])])
		"integer":
			if not _is_integral(value):
				errors.append("'%s' must be an integer" % key)
				return value
			value = int(value)
			_check_range(key, spec, float(value), errors)
		"number":
			if not _is_finite_number(value):
				errors.append("'%s' must be a number" % key)
				return value
			_check_range(key, spec, float(value), errors)
		"boolean":
			if typeof(value) != TYPE_BOOL:
				errors.append("'%s' must be a boolean" % key)
				return value
		"array":
			if not (value is Array):
				errors.append("'%s' must be an array" % key)
				return value
			var items: Array = value
			if spec.has("minItems") and items.size() < int(spec["minItems"]):
				errors.append("'%s' needs at least %d items" % [key, int(spec["minItems"])])
			if spec.has("maxItems") and items.size() > int(spec["maxItems"]):
				errors.append("'%s' allows at most %d items" % [key, int(spec["maxItems"])])
			var item_spec: Dictionary = spec.get("items", {}) if spec.get("items") is Dictionary else {}
			if not item_spec.is_empty():
				var coerced := []
				for index in items.size():
					coerced.append(_coerce("%s[%d]" % [key, index], item_spec, items[index], errors))
				value = coerced
		"object":
			if not (value is Dictionary):
				errors.append("'%s' must be an object" % key)
				return value
		_:
			pass
	if spec.has("enum") and spec["enum"] is Array and not (value in (spec["enum"] as Array)):
		errors.append("'%s' must be one of %s" % [key, JSON.stringify(spec["enum"])])
	return value


static func _check_range(key: String, spec: Dictionary, value: float,
		errors: PackedStringArray) -> void:
	if spec.has("minimum") and value < float(spec["minimum"]):
		errors.append("'%s' must be >= %s" % [key, str(spec["minimum"])])
	if spec.has("maximum") and value > float(spec["maximum"]):
		errors.append("'%s' must be <= %s" % [key, str(spec["maximum"])])


static func _is_finite_number(value: Variant) -> bool:
	return (typeof(value) == TYPE_INT or typeof(value) == TYPE_FLOAT) and is_finite(float(value))


static func _is_integral(value: Variant) -> bool:
	return _is_finite_number(value) and float(value) == floorf(float(value))
