extends GutTest

# McpJson: every engine Variant must come out JSON-encodable and bounded.


func test_primitives_pass_through() -> void:
	assert_eq(McpJson.sanitize(null), null)
	assert_eq(McpJson.sanitize(true), true)
	assert_eq(McpJson.sanitize(42), 42)
	assert_eq(McpJson.sanitize(1.5), 1.5)
	assert_eq(McpJson.sanitize("hello"), "hello")


func test_non_finite_floats_become_strings() -> void:
	assert_true(McpJson.sanitize(INF) is String, "INF is not valid JSON.")
	assert_true(McpJson.sanitize(NAN) is String, "NAN is not valid JSON.")


func test_math_types_become_arrays() -> void:
	assert_eq(McpJson.sanitize(Vector3(1, 2, 3)), [1.0, 2.0, 3.0])
	assert_eq(McpJson.sanitize(Vector2i(4, 5)), [4, 5])
	assert_eq(McpJson.sanitize(Quaternion(0, 0, 0, 1)), [0.0, 0.0, 0.0, 1.0])
	assert_eq(McpJson.sanitize(Color(1, 0, 0, 1)), [1.0, 0.0, 0.0, 1.0])


func test_transform_shape() -> void:
	var out: Dictionary = McpJson.sanitize(Transform3D.IDENTITY)
	assert_true(out.has("basis"))
	assert_eq(out["origin"], [0.0, 0.0, 0.0])
	assert_eq(out["basis"], [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]])


func test_string_name_and_node_path_become_strings() -> void:
	assert_eq(McpJson.sanitize(&"signal_name"), "signal_name")
	assert_eq(McpJson.sanitize(^"a/b"), "a/b")


func test_depth_cap() -> void:
	var nested: Variant = "leaf"
	for i in range(McpJson.MAX_DEPTH + 4):
		nested = [nested]
	var out: Variant = McpJson.sanitize(nested)
	for i in range(McpJson.MAX_DEPTH + 1):
		assert_true(out is Array)
		out = out[0]
	assert_eq(out, "<max depth>", "Recursion stops with an explicit marker.")


func test_array_truncation_marker() -> void:
	var big := []
	big.resize(McpJson.MAX_ENTRIES + 50)
	big.fill(1)
	var out: Array = McpJson.sanitize(big)
	assert_eq(out.size(), McpJson.MAX_ENTRIES + 1, "Capped entries plus one marker.")
	assert_eq(out[McpJson.MAX_ENTRIES], "<truncated 50 more>")


func test_long_string_clipped() -> void:
	var long_text := "x".repeat(McpJson.MAX_STRING + 10)
	var out: String = McpJson.sanitize(long_text)
	assert_true(out.contains("[truncated, 10 more chars]"))


func test_dictionary_keys_become_strings() -> void:
	var out: Dictionary = McpJson.sanitize({ 1: "a", Vector2(0, 0): "b" })
	for key in out:
		assert_true(key is String, "JSON object keys must be strings.")


func test_packed_byte_array_summarized() -> void:
	var bytes := PackedByteArray([1, 2, 3, 4, 5])
	assert_eq(McpJson.sanitize(bytes), "<PackedByteArray 5 bytes>")


func test_packed_float_array_becomes_array() -> void:
	assert_eq(McpJson.sanitize(PackedFloat32Array([1.0, 2.0])), [1.0, 2.0])


func test_node_becomes_path_stub() -> void:
	var node: Node = autofree(Node.new())
	node.name = "Probe"
	var out: Dictionary = McpJson.sanitize(node)
	assert_eq(out["_class"], "Node")
	assert_eq(out["_node"], "Probe", "Out-of-tree nodes report their name.")
	add_child_autofree(node)
	out = McpJson.sanitize(node)
	assert_true(String(out["_node"]).contains("Probe"), "In-tree nodes report their path.")


func test_freed_object_marked() -> void:
	var node := Node.new()
	node.free()
	assert_eq(McpJson.sanitize(node), "<freed object>")


func test_generic_object_stub() -> void:
	var obj := RefCounted.new()
	var out: Dictionary = McpJson.sanitize(obj)
	assert_eq(out["_class"], "RefCounted")
	assert_true(out.has("_string"))


func test_stringify_round_trip() -> void:
	var text := McpJson.stringify({ "pos": Vector3(1, 2, 3), "n": 5 })
	var parsed: Variant = JSON.parse_string(text)
	assert_eq(parsed["pos"], [1.0, 2.0, 3.0])
	assert_eq(parsed["n"], 5.0)
