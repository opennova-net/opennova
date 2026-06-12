extends GutTest

# McpProtocol: version negotiation, envelope shapes, id normalization, and
# message classification.


func test_version_negotiation_matrix() -> void:
	assert_eq(McpProtocol.negotiate_version("2025-06-18"), "2025-06-18", "Latest echoes.")
	assert_eq(McpProtocol.negotiate_version("2025-03-26"), "2025-03-26", "Known older version echoes.")
	assert_eq(McpProtocol.negotiate_version("2024-11-05"), "2025-06-18", "Unknown old version gets our latest.")
	assert_eq(McpProtocol.negotiate_version("2099-01-01"), "2025-06-18", "Unknown future version gets our latest.")
	assert_eq(McpProtocol.negotiate_version(""), "2025-06-18")


func test_result_envelope_shape() -> void:
	var envelope := McpProtocol.result_envelope(7, { "ok": true })
	assert_eq(envelope["jsonrpc"], "2.0")
	assert_eq(envelope["id"], 7)
	assert_eq(envelope["result"], { "ok": true })
	assert_false(envelope.has("error"))


func test_error_envelope_shape() -> void:
	var envelope := McpProtocol.error_envelope("abc", McpProtocol.METHOD_NOT_FOUND, "nope")
	assert_eq(envelope["id"], "abc")
	assert_eq(envelope["error"]["code"], -32601)
	assert_eq(envelope["error"]["message"], "nope")
	assert_false(envelope["error"].has("data"), "data omitted when null.")
	var with_data := McpProtocol.error_envelope(null, McpProtocol.INVALID_PARAMS, "bad", { "field": "x" })
	assert_eq(with_data["error"]["data"], { "field": "x" })


func test_normalize_id_folds_integral_floats() -> void:
	assert_eq(McpProtocol.normalize_id(1.0), 1, "JSON-parsed 1 comes back as int 1.")
	assert_true(McpProtocol.normalize_id(1.0) is int)
	assert_eq(McpProtocol.normalize_id(1.5), 1.5, "Non-integral floats stay floats.")
	assert_eq(McpProtocol.normalize_id("req-1"), "req-1", "String ids untouched.")
	assert_eq(McpProtocol.normalize_id(null), null)


func test_classify_matrix() -> void:
	assert_eq(McpProtocol.classify({ "jsonrpc": "2.0", "id": 1, "method": "ping" }), McpProtocol.MessageKind.REQUEST)
	assert_eq(McpProtocol.classify({ "jsonrpc": "2.0", "method": "notifications/initialized" }), McpProtocol.MessageKind.NOTIFICATION)
	assert_eq(McpProtocol.classify({ "jsonrpc": "2.0", "id": 1, "result": {} }), McpProtocol.MessageKind.RESPONSE)
	assert_eq(McpProtocol.classify({ "jsonrpc": "2.0", "id": 1, "error": { "code": -1, "message": "x" } }), McpProtocol.MessageKind.RESPONSE)
	assert_eq(McpProtocol.classify({ "id": 1, "method": "ping" }), McpProtocol.MessageKind.INVALID, "Missing jsonrpc tag.")
	assert_eq(McpProtocol.classify({ "jsonrpc": "1.0", "id": 1, "method": "ping" }), McpProtocol.MessageKind.INVALID)
	assert_eq(McpProtocol.classify({ "jsonrpc": "2.0", "id": 1, "method": 42 }), McpProtocol.MessageKind.INVALID, "Non-string method.")
	assert_eq(McpProtocol.classify({ "jsonrpc": "2.0" }), McpProtocol.MessageKind.INVALID)
	assert_eq(McpProtocol.classify([1, 2]), McpProtocol.MessageKind.INVALID)
	assert_eq(McpProtocol.classify("ping"), McpProtocol.MessageKind.INVALID)
	assert_eq(McpProtocol.classify(null), McpProtocol.MessageKind.INVALID)
