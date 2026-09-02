class_name DebugArgSpec
extends RefCounted
## One positional argument of a DebugControls action row: its wire name, kind,
## default and the domain a value must lie in. `marshal` is the one validator
## the table's invoke path and the MCP boundary both run, so an argument shape
## is declared once on its row, published by op=list, and refused identically
## everywhere (ADR 0042 d5: every action validates before any engine call).

enum Kind { INT, FLOAT, BOOL, STRING, VECTOR3 }

var name: String
var kind := Kind.INT
var required := true
var default_value: Variant = null
## Numeric domain (per component for VECTOR3); unset = any finite value.
var has_range := false
var minimum := -INF
var maximum := INF
## STRING: the accepted values; empty = any non-blank string.
var choices: PackedStringArray = []


static func integer(arg_name: String) -> DebugArgSpec:
	return _make(arg_name, Kind.INT)


static func number(arg_name: String) -> DebugArgSpec:
	return _make(arg_name, Kind.FLOAT)


static func boolean(arg_name: String) -> DebugArgSpec:
	return _make(arg_name, Kind.BOOL)


static func text(arg_name: String) -> DebugArgSpec:
	return _make(arg_name, Kind.STRING)


## A Vector3, accepted on the wire as Vector3, [x, y, z] or {x, y, z}.
static func vector3(arg_name: String) -> DebugArgSpec:
	return _make(arg_name, Kind.VECTOR3)


static func _make(arg_name: String, arg_kind: Kind) -> DebugArgSpec:
	var spec := DebugArgSpec.new()
	spec.name = arg_name
	spec.kind = arg_kind
	return spec


func between(lo: float, hi: float) -> DebugArgSpec:
	has_range = true
	minimum = lo
	maximum = hi
	return self


func at_least(lo: float) -> DebugArgSpec:
	return between(lo, INF)


func one_of(values: PackedStringArray) -> DebugArgSpec:
	choices = values
	return self


func optional(default: Variant) -> DebugArgSpec:
	required = false
	default_value = default
	return self


func kind_name() -> String:
	match kind:
		Kind.INT:
			return "int"
		Kind.FLOAT:
			return "float"
		Kind.BOOL:
			return "bool"
		Kind.STRING:
			return "string"
		Kind.VECTOR3:
			return "vector3"
	return "unknown"


## The human domain, for refusal messages.
func describe() -> String:
	match kind:
		Kind.INT:
			return "an integer" + _range_text()
		Kind.FLOAT:
			return "a finite number" + _range_text()
		Kind.BOOL:
			return "a boolean"
		Kind.STRING:
			if choices.is_empty():
				return "a non-empty string"
			return "one of " + ", ".join(choices)
		Kind.VECTOR3:
			return "a finite [x, y, z] position" + _range_text()
	return "a value"


## JSON-facing schema row published with the control (op=list).
func to_json_value() -> Dictionary:
	var out := {"name": name, "kind": kind_name(), "required": required}
	if not required:
		out["default"] = default_value
	if has_range and is_finite(minimum):
		out["minimum"] = minimum
	if has_range and is_finite(maximum):
		out["maximum"] = maximum
	if not choices.is_empty():
		out["choices"] = Array(choices)
	return out


## The typed value for one raw wire value, or null when it lies outside the
## spec: numbers are never coerced from strings, floats must be finite,
## booleans must be JSON booleans.
func coerce(raw: Variant) -> Variant:
	match kind:
		Kind.INT:
			if not _is_finite_number(raw) or float(raw) != floorf(float(raw)):
				return null
			return int(raw) if _in_range(float(raw)) else null
		Kind.FLOAT:
			if not _is_finite_number(raw):
				return null
			return float(raw) if _in_range(float(raw)) else null
		Kind.BOOL:
			return raw if typeof(raw) == TYPE_BOOL else null
		Kind.STRING:
			if typeof(raw) != TYPE_STRING:
				return null
			var value := String(raw)
			if value.strip_edges().is_empty():
				return null
			if not choices.is_empty() and not choices.has(value):
				return null
			return value
		Kind.VECTOR3:
			var vector: Variant = _vector3_of(raw)
			if vector == null:
				return null
			var v: Vector3 = vector
			if not (_in_range(v.x) and _in_range(v.y) and _in_range(v.z)):
				return null
			return v
	return null


## Marshal one action's wire arguments against `specs`: null, a positional
## Array, a by-name Dictionary or a single scalar. Returns the typed
## positional Array the row's `invoke` closure reads, or a String saying what
## was wrong (it names the argument).
static func marshal(specs: Array[DebugArgSpec], raw: Variant) -> Variant:
	var positional: Array = []
	if raw is Dictionary:
		var by_name: Dictionary = raw
		for spec in specs:
			if by_name.has(spec.name):
				positional.append(by_name[spec.name])
			elif spec.required:
				return "requires %s (%s)" % [spec.name, spec.describe()]
			else:
				positional.append(spec.default_value)
	elif raw is Array:
		positional = raw
	elif raw != null:
		positional = [raw]
	if positional.size() > specs.size():
		return "takes %d argument(s), got %d" % [specs.size(), positional.size()]
	var out: Array = []
	for i in specs.size():
		var spec := specs[i]
		if i >= positional.size():
			if spec.required:
				return "requires %s (%s)" % [spec.name, spec.describe()]
			out.append(spec.default_value)
			continue
		var value: Variant = spec.coerce(positional[i])
		if value == null:
			return "%s must be %s" % [spec.name, spec.describe()]
		out.append(value)
	return out


func _in_range(value: float) -> bool:
	return not has_range or (value >= minimum and value <= maximum)


func _range_text() -> String:
	if not has_range:
		return ""
	if is_finite(minimum) and is_finite(maximum):
		return " in %s..%s" % [_number_text(minimum), _number_text(maximum)]
	if is_finite(minimum):
		return " >= " + _number_text(minimum)
	return " <= " + _number_text(maximum)


static func _number_text(value: float) -> String:
	return str(int(value)) if value == floorf(value) else str(value)


static func _is_finite_number(value: Variant) -> bool:
	if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
		return false
	return is_finite(float(value))


static func _vector3_of(value: Variant) -> Variant:
	if value is Vector3:
		return value if value.is_finite() else null
	var parts: Array = []
	if value is Array and value.size() == 3:
		parts = value
	elif value is Dictionary and value.has("x") and value.has("y") and value.has("z"):
		parts = [value["x"], value["y"], value["z"]]
	else:
		return null
	for part in parts:
		if not _is_finite_number(part):
			return null
	return Vector3(float(parts[0]), float(parts[1]), float(parts[2]))
