extends GutTest

# McpHttpParser: bytes in, requests out — no sockets. Covers chunk-boundary
# reassembly, pipelining, header normalization, Content-Length framing, the
# rejection statuses (400/411/413/431), and keep-alive defaults.


func _request_bytes(method: String, target: String, headers := {}, body := PackedByteArray(), version := "HTTP/1.1") -> PackedByteArray:
	var text := "%s %s %s\r\n" % [method, target, version]
	var has_length := false
	for name in headers:
		text += "%s: %s\r\n" % [name, headers[name]]
		if String(name).to_lower() == "content-length":
			has_length = true
	if body.size() > 0 and not has_length:
		text += "Content-Length: %d\r\n" % body.size()
	text += "\r\n"
	var bytes := text.to_ascii_buffer()
	bytes.append_array(body)
	return bytes


func test_simple_get_parses() -> void:
	var parser := McpHttpParser.new()
	parser.push(_request_bytes("GET", "/mcp"))
	var request := parser.next_request()
	assert_false(request.is_empty(), "A complete GET parses.")
	assert_eq(request["method"], "GET")
	assert_eq(request["target"], "/mcp")
	assert_eq(request["http_version"], "HTTP/1.1")
	assert_eq((request["body"] as PackedByteArray).size(), 0, "No body on a plain GET.")
	assert_true(request["keep_alive"], "HTTP/1.1 defaults to keep-alive.")
	assert_false(parser.has_error())


func test_request_split_across_arbitrary_chunks() -> void:
	var parser := McpHttpParser.new()
	var body := '{"jsonrpc":"2.0","id":1,"method":"ping"}'.to_utf8_buffer()
	var bytes := _request_bytes("POST", "/mcp", { "Content-Type": "application/json" }, body)
	for i in range(bytes.size()):
		assert_true(parser.next_request().is_empty() or i == bytes.size() - 1, "No request before all bytes arrive.")
		parser.push(bytes.slice(i, i + 1))
	var request := parser.next_request()
	assert_false(request.is_empty(), "Byte-by-byte delivery reassembles.")
	assert_eq(request["method"], "POST")
	assert_eq((request["body"] as PackedByteArray).get_string_from_utf8(), body.get_string_from_utf8())


func test_two_pipelined_requests() -> void:
	var parser := McpHttpParser.new()
	var first := _request_bytes("POST", "/mcp", {}, "one".to_utf8_buffer())
	first.append_array(_request_bytes("POST", "/mcp", {}, "two".to_utf8_buffer()))
	parser.push(first)
	var a := parser.next_request()
	var b := parser.next_request()
	assert_eq((a["body"] as PackedByteArray).get_string_from_utf8(), "one")
	assert_eq((b["body"] as PackedByteArray).get_string_from_utf8(), "two")
	assert_true(parser.next_request().is_empty(), "Nothing left after both.")


func test_header_names_lowercased_last_wins() -> void:
	var parser := McpHttpParser.new()
	parser.push("GET /mcp HTTP/1.1\r\nX-Thing: a\r\nx-THING: b\r\n\r\n".to_ascii_buffer())
	var request := parser.next_request()
	assert_eq(request["headers"]["x-thing"], "b", "Duplicate headers: last value wins under the lowercased key.")


func test_binary_body_preserved() -> void:
	var parser := McpHttpParser.new()
	var body := PackedByteArray()
	for i in range(256):
		body.append(i)
	parser.push(_request_bytes("POST", "/mcp", {}, body))
	var request := parser.next_request()
	assert_eq(request["body"], body, "Body bytes pass through untouched.")


func test_post_without_content_length_has_empty_body() -> void:
	var parser := McpHttpParser.new()
	parser.push("POST /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n".to_ascii_buffer())
	var request := parser.next_request()
	assert_false(request.is_empty())
	assert_eq((request["body"] as PackedByteArray).size(), 0)


func test_chunked_transfer_encoding_rejected_411() -> void:
	var parser := McpHttpParser.new()
	parser.push("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n".to_ascii_buffer())
	assert_true(parser.next_request().is_empty())
	assert_true(parser.has_error())
	assert_eq(parser.error_status(), 411)


func test_oversize_head_rejected_431() -> void:
	var parser := McpHttpParser.new()
	var huge := "GET /mcp HTTP/1.1\r\nX-Pad: ".to_ascii_buffer()
	var pad := PackedByteArray()
	pad.resize(McpHttpParser.MAX_HEAD_BYTES + 64)
	pad.fill(97)
	huge.append_array(pad)
	parser.push(huge)
	assert_true(parser.next_request().is_empty())
	assert_eq(parser.error_status(), 431)


func test_oversize_body_rejected_413() -> void:
	var parser := McpHttpParser.new()
	parser.push(("POST /mcp HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % (McpHttpParser.MAX_BODY_BYTES + 1)).to_ascii_buffer())
	assert_true(parser.next_request().is_empty())
	assert_eq(parser.error_status(), 413)


func test_malformed_request_line_rejected_400() -> void:
	var parser := McpHttpParser.new()
	parser.push("GARBAGE\r\n\r\n".to_ascii_buffer())
	assert_true(parser.next_request().is_empty())
	assert_eq(parser.error_status(), 400)


func test_obs_fold_continuation_rejected_400() -> void:
	var parser := McpHttpParser.new()
	parser.push("GET /mcp HTTP/1.1\r\nX-Thing: a\r\n folded\r\n\r\n".to_ascii_buffer())
	assert_true(parser.next_request().is_empty())
	assert_eq(parser.error_status(), 400)


func test_non_numeric_content_length_rejected_400() -> void:
	var parser := McpHttpParser.new()
	parser.push("POST /mcp HTTP/1.1\r\nContent-Length: abc\r\n\r\n".to_ascii_buffer())
	assert_true(parser.next_request().is_empty())
	assert_eq(parser.error_status(), 400)


func test_keep_alive_defaults() -> void:
	var cases := [
		["HTTP/1.1", {}, true],
		["HTTP/1.1", { "Connection": "close" }, false],
		["HTTP/1.1", { "Connection": "Close" }, false],
		["HTTP/1.0", {}, false],
		["HTTP/1.0", { "Connection": "keep-alive" }, true],
	]
	for case in cases:
		var parser := McpHttpParser.new()
		parser.push(_request_bytes("GET", "/mcp", case[1], PackedByteArray(), case[0]))
		var request := parser.next_request()
		assert_eq(request["keep_alive"], case[2], "%s with %s" % [case[0], case[1]])


func test_leading_crlf_before_request_line_tolerated() -> void:
	var parser := McpHttpParser.new()
	var bytes := "\r\n\r\n".to_ascii_buffer()
	bytes.append_array(_request_bytes("GET", "/mcp"))
	parser.push(bytes)
	var request := parser.next_request()
	assert_false(request.is_empty(), "Leading empty lines are skipped, not parsed as a request.")
	assert_eq(request["method"], "GET")


func test_expect_100_continue_surfaced() -> void:
	var parser := McpHttpParser.new()
	parser.push(_request_bytes("POST", "/mcp", { "Expect": "100-continue" }, "x".to_utf8_buffer()))
	var request := parser.next_request()
	assert_true(request["expects_continue"])
