extends RefCounted

# Loopback HTTP/JSON-RPC client for MCP server tests. Not a *_test.gd, so GUT
# ignores it (nova_menu_shell_probe.gd precedent); tests preload() it. Every wait
# loop awaits process frames so the McpServer node under test keeps polling.

const MAX_WAIT_FRAMES := 600

var peer := StreamPeerTCP.new()
var last_status := 0
var last_headers := {}
var last_body := PackedByteArray()
var _next_id := 1
var _session := ""


func connect_to(tree: SceneTree, port: int) -> bool:
	if peer.connect_to_host("127.0.0.1", port) != OK:
		return false
	for i in range(MAX_WAIT_FRAMES):
		peer.poll()
		match peer.get_status():
			StreamPeerTCP.STATUS_CONNECTED:
				peer.set_no_delay(true)
				return true
			StreamPeerTCP.STATUS_ERROR, StreamPeerTCP.STATUS_NONE:
				return false
		await tree.process_frame
	return false


func close() -> void:
	peer.disconnect_from_host()


## Send one request and read the full response. Returns false on timeout or
## disconnect; on success last_status/last_headers/last_body hold the response.
func request(tree: SceneTree, method: String, path: String, headers := {}, body := PackedByteArray()) -> bool:
	var text := "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n" % [method, path]
	for name in headers:
		text += "%s: %s\r\n" % [name, headers[name]]
	if method == "POST" or body.size() > 0:
		text += "Content-Length: %d\r\n" % body.size()
	text += "\r\n"
	var bytes := text.to_ascii_buffer()
	bytes.append_array(body)
	peer.poll()
	if peer.put_data(bytes) != OK:
		return false
	return await _read_response(tree)


## POST a JSON-RPC payload to /mcp; returns the decoded JSON body (or null for
## empty/invalid bodies — notifications get empty 202s).
func post_json(tree: SceneTree, payload: Variant, extra_headers := {}) -> Variant:
	var headers := { "Content-Type": "application/json", "Accept": "application/json" }
	headers.merge(extra_headers)
	if not await request(tree, "POST", "/mcp", headers, JSON.stringify(payload).to_utf8_buffer()):
		return null
	if last_body.is_empty():
		return null
	return JSON.parse_string(last_body.get_string_from_utf8())


## tools/call-style JSON-RPC request; returns the decoded response envelope.
func rpc(tree: SceneTree, method: String, params := {}, extra_headers := {}) -> Variant:
	var id := _next_id
	_next_id += 1
	return await post_json(tree, { "jsonrpc": "2.0", "id": id, "method": method, "params": params }, extra_headers)


func initialize(tree: SceneTree) -> Variant:
	var envelope: Variant = await rpc(tree, "initialize", {
		"protocolVersion": "2025-06-18",
		"capabilities": {},
		"clientInfo": { "name": "gut-test", "version": "0" },
	})
	if envelope != null:
		await post_json(tree, { "jsonrpc": "2.0", "method": "notifications/initialized" })
	return envelope


func call_tool(tree: SceneTree, name: String, args := {}) -> Variant:
	return await rpc(tree, "tools/call", { "name": name, "arguments": args })


## The most recent Mcp-Session-Id the server issued on this connection (it
## only rides on the initialize response; later responses don't repeat it).
func session_id() -> String:
	return _session


func _read_response(tree: SceneTree) -> bool:
	var buffer := PackedByteArray()
	var head_end := -1
	var content_length := 0
	for i in range(MAX_WAIT_FRAMES):
		peer.poll()
		var available := peer.get_available_bytes()
		while available > 0:
			var chunk := peer.get_partial_data(available)
			if int(chunk[0]) != OK:
				break
			buffer.append_array(chunk[1])
			available = peer.get_available_bytes()
		if head_end < 0:
			head_end = _find_terminator(buffer)
			if head_end >= 0:
				if not _parse_head(buffer.slice(0, head_end).get_string_from_ascii()):
					return false
				content_length = int(String(last_headers.get("content-length", "0")))
		if head_end >= 0 and buffer.size() >= head_end + 4 + content_length:
			last_body = buffer.slice(head_end + 4, head_end + 4 + content_length)
			if last_headers.has("mcp-session-id"):
				_session = String(last_headers["mcp-session-id"])
			return true
		if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED and peer.get_available_bytes() == 0:
			return false
		await tree.process_frame
	return false


static func _find_terminator(buffer: PackedByteArray) -> int:
	var i := buffer.find(13)
	while i >= 0 and i + 3 < buffer.size():
		if buffer[i + 1] == 10 and buffer[i + 2] == 13 and buffer[i + 3] == 10:
			return i
		i = buffer.find(13, i + 1)
	return -1


func _parse_head(text: String) -> bool:
	var lines := text.split("\r\n")
	var status_parts := lines[0].split(" ")
	if status_parts.size() < 2 or not status_parts[0].begins_with("HTTP/"):
		return false
	last_status = status_parts[1].to_int()
	last_headers = {}
	for i in range(1, lines.size()):
		var colon := lines[i].find(":")
		if colon > 0:
			last_headers[lines[i].substr(0, colon).strip_edges().to_lower()] = lines[i].substr(colon + 1).strip_edges()
	return true
