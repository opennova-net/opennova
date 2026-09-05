extends GutTest

# ProbeDef.validate: the JSON-Schema subset game_probe validates a probe's typed
# args against before it starts (defaults filled in, unknown keys rejected,
# integral floats folded to int, enum/range/length/items checks).


func _schema() -> Dictionary:
	return {
		"type": "object",
		"properties": {
			"sample_ms": {"type": "integer", "minimum": 100, "maximum": 60000, "default": 5000},
			"label": {"type": "string", "minLength": 1, "maxLength": 8, "default": "run"},
			"mode": {"type": "string", "enum": ["a", "b"], "default": "a"},
			"scale": {"type": "number", "minimum": 0.0, "maximum": 1.0},
			"verbose": {"type": "boolean", "default": false},
			"ids": {"type": "array", "items": {"type": "integer"}, "maxItems": 3},
			"extra": {"type": "object"},
			"needed": {"type": "string"},
		},
		"required": ["needed"],
	}


func test_defaults_fill_in_and_integral_floats_become_ints() -> void:
	var result := ProbeDef.validate(_schema(), {"needed": "x", "sample_ms": 250.0})
	assert_true(result.ok, str(result.errors))
	assert_eq(result.values["sample_ms"], 250)
	assert_true(result.values["sample_ms"] is int, "JSON 250.0 reads as the integer 250")
	assert_eq(result.values["label"], "run")
	assert_eq(result.values["mode"], "a")
	assert_eq(result.values["verbose"], false)
	assert_false(result.values.has("scale"), "no default, not supplied: absent")
	assert_eq(result.values["needed"], "x")


func test_unknown_keys_and_missing_required_are_errors() -> void:
	var result := ProbeDef.validate(_schema(), {"bogus": 1})
	assert_false(result.ok)
	assert_true("unknown argument 'bogus'" in result.errors, str(result.errors))
	assert_true("missing required argument 'needed'" in result.errors, str(result.errors))
	var empty := ProbeDef.validate(_schema(), null)
	assert_false(empty.ok, "null args read as {} and still miss the required key")


func test_type_enum_and_range_violations() -> void:
	var result := ProbeDef.validate(_schema(), {
		"needed": 7, "sample_ms": 2.5, "label": "", "mode": "c", "scale": 1.5, "verbose": "yes",
	})
	assert_false(result.ok)
	for expected in [
		"'needed' must be a string", "'sample_ms' must be an integer",
		"'label' must be at least 1 characters", "'mode' must be one of [\"a\",\"b\"]",
		"'scale' must be <= 1.0", "'verbose' must be a boolean",
	]:
		assert_true(expected in result.errors, "%s in %s" % [expected, str(result.errors)])
	var low := ProbeDef.validate(_schema(), {"needed": "x", "sample_ms": 10})
	assert_true("'sample_ms' must be >= 100" in low.errors, str(low.errors))


func test_array_items_and_object_pass_through() -> void:
	var ok := ProbeDef.validate(_schema(), {"needed": "x", "ids": [1, 2.0], "extra": {"k": [1]}})
	assert_true(ok.ok, str(ok.errors))
	assert_eq(ok.values["ids"], [1, 2])
	assert_true(ok.values["ids"][1] is int)
	assert_eq(ok.values["extra"], {"k": [1]})
	var bad := ProbeDef.validate(_schema(), {"needed": "x", "ids": [1, "two", 3, 4], "extra": 3})
	assert_false(bad.ok)
	assert_true("'ids[1]' must be an integer" in bad.errors, str(bad.errors))
	assert_true("'ids' allows at most 3 items" in bad.errors, str(bad.errors))
	assert_true("'extra' must be an object" in bad.errors, str(bad.errors))


func test_non_object_input_is_rejected() -> void:
	var result := ProbeDef.validate(_schema(), [1, 2])
	assert_false(result.ok)
	assert_eq(result.errors, PackedStringArray(["args must be a JSON object"]))
	assert_true(result.values.is_empty())


func test_untyped_property_accepts_anything() -> void:
	var result := ProbeDef.validate({"type": "object", "properties": {"any": {}}}, {"any": [1, {"a": 2}]})
	assert_true(result.ok, str(result.errors))
	assert_eq(result.values["any"], [1, {"a": 2}])
