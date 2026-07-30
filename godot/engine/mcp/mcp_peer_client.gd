class_name McpPeerClient
extends RefCounted

## Small Streamable-HTTP MCP client for trusted loopback peers. ONED uses one
## to proxy its stable authoring endpoint to the ephemeral MCP endpoint in the
## editor-launched game. It deliberately implements only the protocol subset
## McpServer exposes: initialize, notifications/initialized and tools/call.
##
## The client is serial by construction. A ShellGameSession owns one instance
## and its proxy tools run through ONED's serial tool queue, so no request can
## interleave bytes with another on the StreamPeerTCP connection.

const DEFAULT_TIMEOUT_MS := 10_000
const PROTOCOL_VERSION := "2025-06-18"

var _peer := StreamPeerTCP.new()
var _tree: SceneTree = null
var _host := "127.0.0.1"
var _port := 0
var _path := McpServer.MCP_PATH
var _session_id := ""
var _next_id := 1
var _last_error := ""


func connect_to_url(url: String, tree: SceneTree,
		timeout_ms := DEFAULT_TIMEOUT_MS) -> Error:
	close()
	_tree = tree
	var endpoint := _parse_loopback_url(url)
	if endpoint.is_empty():
		_last_error = "Invalid runtime MCP URL: %s" % url
		return ERR_INVALID_PARAMETER
	_host = endpoint["host"]
	_port = endpoint["port"]
	_path = endpoint["path"]
	var err := _peer.connect_to_host(_host, _port)
	if err != OK:
		_last_error = "Could not connect to runtime MCP at %s: %s" % [
			url, error_string(err)]
		return err
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		_peer.poll()
		match _peer.get_status():
			StreamPeerTCP.STATUS_CONNECTED:
				_peer.set_no_delay(true)
				var remaining := maxi(
						1, int(deadline - Time.get_ticks_msec()))
				var initialized: Variant = await _rpc("initialize", {
					"protocolVersion": PROTOCOL_VERSION,
					"capabilities": {},
					"clientInfo": {
						"name": "oned-game-proxy",
						"version": str(ProjectSettings.get_setting(
								"application/config/version", "0.0.0")),
					},
				}, remaining)
				if initialized == null:
					close()
					return ERR_CANT_CONNECT
				remaining = maxi(1, int(deadline - Time.get_ticks_msec()))
				if await _notify(
						"notifications/initialized", {}, remaining) != OK:
					close()
					return ERR_CANT_CONNECT
				_last_error = ""
				return OK
			StreamPeerTCP.STATUS_ERROR, StreamPeerTCP.STATUS_NONE:
				_last_error = "Runtime MCP disconnected while connecting to %s." % url
				close()
				return ERR_CANT_CONNECT
		if _tree == null:
			break
		await _tree.process_frame
	_last_error = "Timed out connecting to runtime MCP at %s." % url
	close()
	return ERR_TIMEOUT


func close() -> void:
	_peer.disconnect_from_host()
	_session_id = ""
	_port = 0


func has_connection() -> bool:
	_peer.poll()
	return _peer.get_status() == StreamPeerTCP.STATUS_CONNECTED


func get_last_error() -> String:
	return _last_error


func call_tool(name: String, args := {},
		timeout_ms := DEFAULT_TIMEOUT_MS) -> McpToolResult:
	if not has_connection():
		return McpToolResult.error(
				"The game is running without a runtime debug connection.")
	var envelope: Variant = await _rpc("tools/call", {
		"name": name,
		"arguments": args if args is Dictionary else {},
	}, timeout_ms)
	if envelope == null:
		return McpToolResult.error(_last_error)
	if not (envelope is Dictionary):
		_last_error = "Runtime MCP returned a malformed JSON-RPC response."
		close()
		return McpToolResult.error("Runtime MCP returned a malformed response.")
	if envelope.has("error"):
		var remote_error: Variant = envelope["error"]
		var message := String(remote_error.get("message", "Runtime MCP request failed.")) \
				if remote_error is Dictionary else "Runtime MCP request failed."
		return McpToolResult.error(message, remote_error)
	if not (envelope.get("result") is Dictionary):
		_last_error = "Runtime MCP returned no result for '%s'." % name
		close()
		return McpToolResult.error(
				_last_error)
	return McpToolResult.from_payload(envelope["result"])


## Queue one tools/call request without waiting for its response. Used only for
## editor shutdown/stop: the managed session gives the child a grace window,
## then falls back to terminating the PID.
func send_tool_no_wait(name: String, args := {}) -> Error:
	if not has_connection():
		return ERR_UNAVAILABLE
	var id := _next_id
	_next_id += 1
	return _send_json({
		"jsonrpc": "2.0",
		"id": id,
		"method": "tools/call",
		"params": {
			"name": name,
			"arguments": args if args is Dictionary else {},
		},
	})


func _rpc(method: String, params: Dictionary,
		timeout_ms: int) -> Variant:
	var id := _next_id
	_next_id += 1
	var envelope: Variant = await _post_json({
		"jsonrpc": "2.0",
		"id": id,
		"method": method,
		"params": params,
	}, timeout_ms)
	if envelope == null:
		return null
	if not (envelope is Dictionary):
		_last_error = "Runtime MCP returned a malformed JSON-RPC response."
		close()
		return null
	var response_id: Variant = envelope.get("id")
	if typeof(response_id) not in [TYPE_INT, TYPE_FLOAT] \
			or int(response_id) != id:
		_last_error = "Runtime MCP returned a response for the wrong request."
		close()
		return null
	return envelope


func _notify(method: String, params: Dictionary,
		timeout_ms: int) -> Error:
	var response: Variant = await _post_json({
		"jsonrpc": "2.0",
		"method": method,
		"params": params,
	}, timeout_ms)
	return OK if response != null else ERR_CANT_CONNECT


func _post_json(payload: Dictionary, timeout_ms: int) -> Variant:
	var send_error := _send_json(payload)
	if send_error != OK:
		close()
		return null
	var response := await _read_response(timeout_ms)
	if not bool(response.get("ok", false)):
		close()
		return null
	var response_body: PackedByteArray = response["body"]
	if response_body.is_empty():
		return {}
	var json := JSON.new()
	if json.parse(response_body.get_string_from_utf8()) != OK:
		_last_error = "Runtime MCP returned invalid JSON: %s" % json.get_error_message()
		close()
		return null
	return json.data


func _send_json(payload: Dictionary) -> Error:
	var body := JSON.stringify(payload).to_utf8_buffer()
	var request := "POST %s HTTP/1.1\r\n" % _path
	request += "Host: %s:%d\r\n" % [_host, _port]
	request += "Content-Type: application/json\r\n"
	request += "Accept: application/json\r\n"
	request += "Content-Length: %d\r\n" % body.size()
	if not _session_id.is_empty():
		request += "Mcp-Session-Id: %s\r\n" % _session_id
	request += "\r\n"
	var bytes := request.to_ascii_buffer()
	bytes.append_array(body)
	_peer.poll()
	var put := _peer.put_data(bytes)
	if put != OK:
		_last_error = "Could not send a request to runtime MCP: %s" % error_string(put)
		return put
	return OK


func _read_response(timeout_ms: int) -> Dictionary:
	var buffer := PackedByteArray()
	var head_end := -1
	var content_length := 0
	var status := 0
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		_peer.poll()
		var available := _peer.get_available_bytes()
		while available > 0:
			var chunk := _peer.get_partial_data(available)
			if int(chunk[0]) != OK:
				_last_error = "Runtime MCP response read failed: %s" % \
						error_string(int(chunk[0]))
				return {"ok": false}
			buffer.append_array(chunk[1])
			available = _peer.get_available_bytes()
		if head_end < 0:
			head_end = _find_terminator(buffer)
			if head_end >= 0:
				var parsed := _parse_head(
						buffer.slice(0, head_end).get_string_from_ascii())
				if parsed.is_empty():
					return {"ok": false}
				status = parsed["status"]
				content_length = parsed["content_length"]
				if not String(parsed["session_id"]).is_empty():
					_session_id = parsed["session_id"]
		if head_end >= 0 and buffer.size() >= head_end + 4 + content_length:
			if status < 200 or status >= 300:
				_last_error = "Runtime MCP returned HTTP %d." % status
				return {"ok": false}
			return {
				"ok": true,
				"body": buffer.slice(
						head_end + 4, head_end + 4 + content_length),
			}
		if _peer.get_status() != StreamPeerTCP.STATUS_CONNECTED \
				and _peer.get_available_bytes() == 0:
			_last_error = "Runtime MCP disconnected before responding."
			return {"ok": false}
		if _tree == null:
			break
		await _tree.process_frame
	_last_error = "Runtime MCP did not respond within %d ms." % timeout_ms
	return {"ok": false}


static func _find_terminator(buffer: PackedByteArray) -> int:
	var i := buffer.find(13)
	while i >= 0 and i + 3 < buffer.size():
		if buffer[i + 1] == 10 and buffer[i + 2] == 13 \
				and buffer[i + 3] == 10:
			return i
		i = buffer.find(13, i + 1)
	return -1


func _parse_head(text: String) -> Dictionary:
	var lines := text.split("\r\n")
	var status_parts := lines[0].split(" ")
	if status_parts.size() < 2 or not status_parts[0].begins_with("HTTP/"):
		_last_error = "Runtime MCP returned a malformed HTTP response."
		return {}
	var headers := {}
	for i in range(1, lines.size()):
		var colon := lines[i].find(":")
		if colon > 0:
			headers[lines[i].substr(0, colon).strip_edges().to_lower()] = \
					lines[i].substr(colon + 1).strip_edges()
	var length_text := String(headers.get("content-length", "0"))
	if not length_text.is_valid_int():
		_last_error = "Runtime MCP response has an invalid Content-Length."
		return {}
	return {
		"status": status_parts[1].to_int(),
		"content_length": length_text.to_int(),
		"session_id": String(headers.get("mcp-session-id", "")),
	}


static func _parse_loopback_url(url: String) -> Dictionary:
	var text := url.strip_edges()
	if not text.begins_with("http://"):
		return {}
	text = text.trim_prefix("http://")
	var authority := text.get_slice("/", 0)
	var path := "/" + text.trim_prefix(authority).trim_prefix("/")
	var host := authority.get_slice(":", 0).to_lower()
	var port_text := authority.get_slice(":", 1)
	if host not in ["127.0.0.1", "localhost"] or not port_text.is_valid_int():
		return {}
	var port := port_text.to_int()
	if port <= 0 or port > 65535:
		return {}
	return {
		"host": host,
		"port": port,
		"path": path if path != "/" else McpServer.MCP_PATH,
	}
