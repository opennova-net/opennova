class_name McpJson
extends RefCounted

## Variant → JSON-safe sanitizer for MCP tool results. Agents get back whatever
## a tool handler returns, so this has to
## turn ANY engine value into something JSON.stringify can encode without
## errors and without flooding the context window: math types become arrays,
## nodes become path stubs, collections and strings are capped with explicit
## truncation markers, and cycles are cut by the depth limit.

# Deep enough for honestly-nested tool data (menu widget trees easily reach
# depth 9+: screens → tree → children → window → ... → rect array) while still
# cutting accidental cycles fast.
const MAX_DEPTH := 24
const MAX_ENTRIES := 200
const MAX_STRING := 8192


static func sanitize(value: Variant, depth := 0) -> Variant:
	if depth > MAX_DEPTH:
		return "<max depth>"
	match typeof(value):
		TYPE_NIL:
			return null
		TYPE_BOOL, TYPE_INT:
			return value
		TYPE_FLOAT:
			return value if is_finite(value) else str(value)
		TYPE_STRING:
			return _clip(value)
		TYPE_STRING_NAME, TYPE_NODE_PATH:
			return _clip(String(value))
		TYPE_VECTOR2, TYPE_VECTOR2I:
			return [value.x, value.y]
		TYPE_VECTOR3, TYPE_VECTOR3I:
			return [value.x, value.y, value.z]
		TYPE_VECTOR4, TYPE_VECTOR4I, TYPE_QUATERNION:
			return [value.x, value.y, value.z, value.w]
		TYPE_COLOR:
			return [value.r, value.g, value.b, value.a]
		TYPE_RECT2, TYPE_RECT2I, TYPE_AABB:
			return { "position": sanitize(value.position, depth + 1), "size": sanitize(value.size, depth + 1) }
		TYPE_PLANE:
			return { "normal": sanitize(value.normal, depth + 1), "d": value.d }
		TYPE_BASIS:
			return [sanitize(value.x, depth + 1), sanitize(value.y, depth + 1), sanitize(value.z, depth + 1)]
		TYPE_TRANSFORM2D:
			return { "x": sanitize(value.x, depth + 1), "y": sanitize(value.y, depth + 1), "origin": sanitize(value.origin, depth + 1) }
		TYPE_TRANSFORM3D:
			return { "basis": sanitize(value.basis, depth + 1), "origin": sanitize(value.origin, depth + 1) }
		TYPE_PACKED_BYTE_ARRAY:
			return "<PackedByteArray %d bytes>" % value.size()
		TYPE_PACKED_INT32_ARRAY, TYPE_PACKED_INT64_ARRAY, TYPE_PACKED_FLOAT32_ARRAY, \
		TYPE_PACKED_FLOAT64_ARRAY, TYPE_PACKED_STRING_ARRAY, TYPE_PACKED_VECTOR2_ARRAY, \
		TYPE_PACKED_VECTOR3_ARRAY, TYPE_PACKED_VECTOR4_ARRAY, TYPE_PACKED_COLOR_ARRAY:
			return _sanitize_list(value, value.size(), depth)
		TYPE_ARRAY:
			return _sanitize_list(value, value.size(), depth)
		TYPE_DICTIONARY:
			return _sanitize_dictionary(value, depth)
		TYPE_OBJECT:
			return _sanitize_object(value, depth)
		TYPE_CALLABLE, TYPE_SIGNAL, TYPE_RID:
			return str(value)
	return str(value)


## sanitize() + stringify in one step, for callers that want the final text.
static func stringify(value: Variant, indent := "\t") -> String:
	return JSON.stringify(sanitize(value), indent)


static func _clip(text: String) -> String:
	if text.length() <= MAX_STRING:
		return text
	return text.substr(0, MAX_STRING) + "...[truncated, %d more chars]" % (text.length() - MAX_STRING)


static func _sanitize_list(values: Variant, size: int, depth: int) -> Array:
	var out := []
	var count := mini(size, MAX_ENTRIES)
	for i in range(count):
		out.append(sanitize(values[i], depth + 1))
	if size > MAX_ENTRIES:
		out.append("<truncated %d more>" % (size - MAX_ENTRIES))
	return out


static func _sanitize_dictionary(value: Dictionary, depth: int) -> Dictionary:
	var out := {}
	var count := 0
	for key in value:
		if count >= MAX_ENTRIES:
			out["<truncated>"] = "%d more entries" % (value.size() - MAX_ENTRIES)
			break
		out[str(key)] = sanitize(value[key], depth + 1)
		count += 1
	return out


static func _sanitize_object(value: Variant, _depth: int) -> Variant:
	if not is_instance_valid(value):
		return "<freed object>"
	if value is Node:
		var node: Node = value
		var path := String(node.get_path()) if node.is_inside_tree() else String(node.name)
		return { "_class": node.get_class(), "_node": _clip(path) }
	if value is Image:
		var img: Image = value
		return { "_class": "Image", "width": img.get_width(), "height": img.get_height(), "format": img.get_format() }
	return { "_class": value.get_class(), "_string": _clip(str(value)) }
