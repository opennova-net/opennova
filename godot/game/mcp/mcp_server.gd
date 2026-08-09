class_name McpServer
extends Node

## The embedded MCP server: a loopback-only Streamable HTTP endpoint speaking
## JSON-RPC 2.0, polled from _process on the main thread. Protocol-only
## requests (initialize, ping, tools/list) answer same-frame; tools/call runs
## through a serialized FIFO so tool handlers never re-enter editor state —
## except tools registered serial = false (read-only monitors), which run
## immediately alongside a queued job.
##
## v1 deliberately omits SSE: every POST gets one application/json response,
## GET /mcp is 405, and notifications can't be pushed (tools list changes are
## picked up when a client re-lists). McpHttpConnection.respond is the seam to
## grow an SSE stream on later.
##
## The host wires `registry`, `context_factory`, `instructions`, and
## server identity, then start()s it. Tests start(0) for an ephemeral port.

const MCP_PATH := "/mcp"
const MAX_CONNECTIONS := 16
const MAX_SESSIONS := 32
const SESSION_IDLE_MS := 8 * 3600 * 1000

var registry := McpToolRegistry.new()
var instructions := ""
var server_name := "oned"
var server_title := "OpenNova Editor (ONED)"
## Disabled only by tests that exercise non-localhost behavior.
var origin_check_enabled := true
## func(args: Dictionary) -> McpToolContext; when invalid, a bare context is
## built (pure tests, no editor).
var context_factory: Callable = Callable()
## Optional sink for server log lines: func(text: String).
var log_sink: Callable = Callable()

var _tcp := TCPServer.new()
var _connections: Array = []
var _sessions := {}
var _tool_queue: Array = []
var _tool_running := false
var _active_ctx: McpToolContext = null
var _active_request_id: Variant = null


func start(port: int, bind_address := "127.0.0.1") -> Error:
	if _tcp.is_listening():
		return ERR_ALREADY_IN_USE
	return _tcp.listen(port, bind_address)


func stop() -> void:
	for conn in _connections:
		conn.close()
	_connections.clear()
	_tool_queue.clear()
	if _tcp.is_listening():
		_tcp.stop()


func is_running() -> bool:
	return _tcp.is_listening()


## The bound port (start(0) binds an ephemeral one; this reports it).
func get_port() -> int:
	return _tcp.get_local_port() if _tcp.is_listening() else 0


func get_url() -> String:
	return "http://127.0.0.1:%d%s" % [get_port(), MCP_PATH] if is_running() else ""


## The session record for an Mcp-Session-Id (creating is initialize-only);
## {} when unknown. Hosts use this for per-session state like log cursors.
func session(id: String) -> Dictionary:
	return _sessions.get(id, {})


func _exit_tree() -> void:
	stop()


func _process(_delta: float) -> void:
	if not _tcp.is_listening():
		return
	var now := Time.get_ticks_msec()
	_accept_connections(now)
	for conn: McpHttpConnection in _connections.duplicate():
		var request: Dictionary = conn.poll(now)
		if conn.parser_error_status() != 0:
			_respond_parser_error(conn)
			continue
		if not request.is_empty():
			_route(conn, request)
	for conn: McpHttpConnection in _connections.duplicate():
		if not conn.busy and (not conn.is_alive() or conn.idle_too_long(now)):
			conn.close()
			_connections.erase(conn)
	if not _tool_running and not _tool_queue.is_empty():
		_run_tool_job(_tool_queue.pop_front(), true)
	_prune_sessions(now)


func _accept_connections(now: int) -> void:
	while _tcp.is_connection_available():
		var socket := _tcp.take_connection()
		if socket == null:
			break
		var conn := McpHttpConnection.new()
		conn.setup(socket, now)
		if _connections.size() >= MAX_CONNECTIONS:
			conn.respond_json(503, McpProtocol.error_envelope(null, McpProtocol.INTERNAL_ERROR,
					"Too many connections"), {}, false)
			continue
		_connections.append(conn)


func _respond_parser_error(conn: McpHttpConnection) -> void:
	var status := conn.parser_error_status()
	conn.respond_json(status, McpProtocol.error_envelope(null, McpProtocol.INVALID_REQUEST,
			"Malformed HTTP request (%d)" % status), {}, false)
	_connections.erase(conn)


func _route(conn: McpHttpConnection, request: Dictionary) -> void:
	if not _origin_allowed(request["headers"]):
		_log("rejected request with non-local Origin/Host")
		conn.respond_json(403, McpProtocol.error_envelope(null, McpProtocol.INVALID_REQUEST,
				"Forbidden: only local clients are allowed"), {}, false)
		_connections.erase(conn)
		return
	var path := String(request["target"]).get_slice("?", 0)
	if path != MCP_PATH:
		conn.respond_json(404, McpProtocol.error_envelope(null, McpProtocol.INVALID_REQUEST,
				"Not found — the MCP endpoint is %s" % MCP_PATH), {}, request["keep_alive"])
		return
	match String(request["method"]):
		"POST":
			_handle_post(conn, request)
		"DELETE":
			_sessions.erase(_session_id_of(request))
			conn.respond_empty(200, {}, request["keep_alive"])
		_:
			conn.respond_json(405, McpProtocol.error_envelope(null, McpProtocol.INVALID_REQUEST,
					"Method not allowed (no SSE stream in this server)"), { "Allow": "POST, DELETE" },
					request["keep_alive"])


func _handle_post(conn: McpHttpConnection, request: Dictionary) -> void:
	var body: PackedByteArray = request["body"]
	# JSON.new().parse stays silent on bad input (parse_string prints an
	# engine error per failure, which would spam the log on every bad client).
	var json := JSON.new()
	if json.parse(body.get_string_from_utf8()) != OK:
		conn.respond_json(400, McpProtocol.error_envelope(null, McpProtocol.PARSE_ERROR,
				"Body is not valid JSON: %s" % json.get_error_message()), {}, request["keep_alive"])
		return
	var message: Variant = json.data
	if message is Array:
		conn.respond_json(400, McpProtocol.error_envelope(null, McpProtocol.INVALID_REQUEST,
				"JSON-RPC batching is not supported (MCP 2025-06-18 removed it)"), {}, request["keep_alive"])
		return
	match McpProtocol.classify(message):
		McpProtocol.MessageKind.NOTIFICATION:
			_handle_notification(message)
			conn.respond_empty(202, {}, request["keep_alive"])
		McpProtocol.MessageKind.RESPONSE:
			# We never issue server -> client requests in v1; ack and ignore.
			conn.respond_empty(202, {}, request["keep_alive"])
		McpProtocol.MessageKind.REQUEST:
			_dispatch(conn, request, message)
		_:
			conn.respond_json(400, McpProtocol.error_envelope(message.get("id") if message is Dictionary else null,
					McpProtocol.INVALID_REQUEST, "Invalid JSON-RPC message"), {}, request["keep_alive"])


func _handle_notification(message: Dictionary) -> void:
	if String(message["method"]) != "notifications/cancelled":
		return
	var params: Dictionary = message.get("params", {}) if message.get("params") is Dictionary else {}
	var request_id: Variant = McpProtocol.normalize_id(params.get("requestId"))
	if request_id == null:
		return
	for job: Dictionary in _tool_queue.duplicate():
		if job["id"] == request_id:
			_tool_queue.erase(job)
			var conn: McpHttpConnection = job["conn"]
			conn.respond_json(200, McpProtocol.error_envelope(request_id, McpProtocol.REQUEST_CANCELLED,
					"Request cancelled before it ran"), {}, job["keep_alive"])
			return
	if _active_ctx != null and _active_request_id == request_id:
		# Best-effort cooperative cancellation; the tool's response is still
		# sent when it returns (clients discard responses to cancelled ids).
		_active_ctx.cancelled = true


func _dispatch(conn: McpHttpConnection, request: Dictionary, message: Dictionary) -> void:
	var id: Variant = McpProtocol.normalize_id(message.get("id"))
	var params: Dictionary = message.get("params", {}) if message.get("params") is Dictionary else {}
	var keep_alive: bool = request["keep_alive"]
	_touch_session(_session_id_of(request))
	match String(message["method"]):
		"initialize":
			_handle_initialize(conn, id, params, keep_alive)
		"ping":
			conn.respond_json(200, McpProtocol.result_envelope(id, {}), {}, keep_alive)
		"tools/list":
			conn.respond_json(200, McpProtocol.result_envelope(id, { "tools": registry.list_tools() }), {}, keep_alive)
		"tools/call":
			_handle_tool_call(conn, request, id, params)
		_:
			conn.respond_json(200, McpProtocol.error_envelope(id, McpProtocol.METHOD_NOT_FOUND,
					"Method not found: %s" % message["method"]), {}, keep_alive)


func _handle_initialize(conn: McpHttpConnection, id: Variant, params: Dictionary, keep_alive: bool) -> void:
	var negotiated := McpProtocol.negotiate_version(String(params.get("protocolVersion", "")))
	var session_id := Crypto.new().generate_random_bytes(16).hex_encode()
	_sessions[session_id] = {
		"created_ms": Time.get_ticks_msec(),
		"last_seen_ms": Time.get_ticks_msec(),
		"protocol_version": negotiated,
		"log_cursor": 0,
	}
	var result := {
		"protocolVersion": negotiated,
		"capabilities": { "tools": { "listChanged": false } },
		"serverInfo": {
			"name": server_name,
			"title": server_title,
			"version": str(ProjectSettings.get_setting("application/config/version", "0.0.0")),
		},
	}
	if not instructions.is_empty():
		result["instructions"] = instructions
	_log("session %s initialized (protocol %s)" % [session_id.substr(0, 8), negotiated])
	conn.respond_json(200, McpProtocol.result_envelope(id, result), { "Mcp-Session-Id": session_id }, keep_alive)


func _handle_tool_call(conn: McpHttpConnection, request: Dictionary, id: Variant, params: Dictionary) -> void:
	var name := String(params.get("name", ""))
	var args: Dictionary = params.get("arguments", {}) if params.get("arguments") is Dictionary else {}
	if not registry.has_tool(name):
		conn.respond_json(200, McpProtocol.error_envelope(id, McpProtocol.INVALID_PARAMS,
				"Unknown tool: %s" % name), {}, request["keep_alive"])
		return
	var job := {
		"conn": conn,
		"id": id,
		"name": name,
		"args": args,
		"keep_alive": request["keep_alive"],
		"session_id": _session_id_of(request),
	}
	conn.busy = true
	if registry.is_serial(name):
		_tool_queue.append(job)
	else:
		_run_tool_job(job, false)


# Fire-and-forget coroutine: runs one tool job to completion and responds.
# registry.call_tool's watchdog guarantees it returns, so a serial job always
# releases the queue.
func _run_tool_job(job: Dictionary, serial: bool) -> void:
	if serial:
		_tool_running = true
	var ctx := _make_context(job["args"], job["session_id"])
	if serial:
		_active_ctx = ctx
		_active_request_id = job["id"]
	var result: McpToolResult = await registry.call_tool(job["name"], job["args"], ctx)
	if serial:
		_tool_running = false
		_active_ctx = null
		_active_request_id = null
	var conn: McpHttpConnection = job["conn"]
	if conn.is_alive():
		conn.respond_json(200, McpProtocol.result_envelope(job["id"], result.to_payload()), {}, job["keep_alive"])
	else:
		conn.busy = false
		_log("tools/call %s finished after the client disconnected; response dropped" % job["name"])


func _make_context(args: Dictionary, session_id: String) -> McpToolContext:
	var ctx: McpToolContext = null
	if context_factory.is_valid():
		ctx = context_factory.call(args)
	if ctx == null:
		ctx = McpToolContext.new()
	ctx.args = args
	if ctx.tree == null:
		ctx.tree = get_tree()
	ctx.args["_session_id"] = session_id
	return ctx


func _session_id_of(request: Dictionary) -> String:
	return String(request["headers"].get("mcp-session-id", ""))


func _touch_session(id: String) -> void:
	if not id.is_empty() and _sessions.has(id):
		_sessions[id]["last_seen_ms"] = Time.get_ticks_msec()


func _prune_sessions(now: int) -> void:
	if _sessions.size() <= MAX_SESSIONS:
		var stale: Array = []
		for id in _sessions:
			if now - int(_sessions[id]["last_seen_ms"]) > SESSION_IDLE_MS:
				stale.append(id)
		for id in stale:
			_sessions.erase(id)
		return
	var ids := _sessions.keys()
	ids.sort_custom(func(a, b): return int(_sessions[a]["last_seen_ms"]) < int(_sessions[b]["last_seen_ms"]))
	while _sessions.size() > MAX_SESSIONS:
		_sessions.erase(ids.pop_front())


# Only local clients: Origin (when sent) and Host (when sent) must name
# localhost. Browsers always send Origin on cross-origin fetches, so this is
# the DNS-rebinding gate; curl and MCP clients typically send neither Origin
# nor a non-local Host.
func _origin_allowed(headers: Dictionary) -> bool:
	if not origin_check_enabled:
		return true
	for key in ["origin", "host"]:
		var raw := String(headers.get(key, ""))
		if raw.is_empty():
			continue
		if not _is_local_host(raw):
			return false
	return true


static func _is_local_host(raw: String) -> bool:
	var host := raw.to_lower()
	if host.contains("://"):
		host = host.get_slice("://", 1)
	host = host.get_slice("/", 0)
	if host.begins_with("["):
		host = host.get_slice("]", 0).trim_prefix("[")
	else:
		host = host.get_slice(":", 0)
	return host in ["localhost", "127.0.0.1", "::1"]


func _log(text: String) -> void:
	if log_sink.is_valid():
		log_sink.call(text)
