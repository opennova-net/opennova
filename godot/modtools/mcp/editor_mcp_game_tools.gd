class_name EditorMcpGameTools
extends RefCounted

## Stable ONED-side adapter for the editor-managed standalone game. The editor
## owns only process lifecycle and a loopback proxy; all runtime state and
## debug controls live in the real game process.

const DESCRIPTOR_WAIT_MS := 15_000
const PEER_RESPONSE_MARGIN_MS := 2_000
const PROXY_WATCHDOG_MARGIN_MS := 1_000
const ENDPOINT_SHUTDOWN_WAIT_MS := 2_000
## A reserved endpoint-retirement request joins the child server's global
## serial FIFO. Let the longest already-running tool reach its watchdog before
## treating retirement as failed; otherwise disabling Agent during a slow
## screenshot can falsely trigger the managed-game safety stop.
const ENDPOINT_RETIRE_CALL_TIMEOUT_MS := \
		GameMcpCatalog.SCREENSHOT_TIMEOUT_MS + PEER_RESPONSE_MARGIN_MS \
		+ PROXY_WATCHDOG_MARGIN_MS

var service: Node
var _game_run: EditorGameRunBridge
var _peer: McpPeerClient
var _connected_run_id: String
var _last_error := ""
var _log_cursors: Dictionary = {}
var _runtime_gate_generation := 0
var _runtime_debug_authorized := false


func _init(
		mcp_service: Node,
		game_run: EditorGameRunBridge = null,
		peer: McpPeerClient = null,
		connected_run_id := "") -> void:
	service = mcp_service
	_game_run = game_run if game_run != null else EditorGameRunBridge.new()
	_peer = peer if peer != null else McpPeerClient.new()
	_connected_run_id = connected_run_id


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("run_game",
			"Manage ONED's one standalone game process. op=start restarts the current child in "
			+ "mode=game (F5: normal boot) or mode=mission (F6: current saved top-level loose .bms); "
			+ "op=stop is F8; op=state polls it. Running never saves, exports, copies, or stages "
			+ "editor data. Unsaved edits are excluded.",
			{
				"op": {"type": "string", "enum": ["start", "stop", "state"]},
				"mode": {"type": "string", "enum": ["game", "mission"], "default": "game"},
			}, ["op"]), Callable(self, "_tool_run_game"))
	for def in GameMcpCatalog.definitions():
		# One persistent Streamable-HTTP connection backs the proxy. Keep every
		# forwarded call on ONED's FIFO even when the child-side read is safe to
		# run concurrently with other child clients.
		def.serial = true
		# The stable editor endpoint may spend time waiting for the child
		# descriptor before the child-side tool budget starts.
		def.timeout_ms += DESCRIPTOR_WAIT_MS \
				+ PEER_RESPONSE_MARGIN_MS + PROXY_WATCHDOG_MARGIN_MS
		registry.register(def, Callable(self, "_proxy_%s" % def.name))


func bind_session() -> void:
	var session := _session()
	if session != null:
		session.set_runtime_control_hooks(
				Callable(self, "forward_runtime_tool"),
				Callable(self, "request_runtime_quit"))


## A live Agent server is the sole authority for adding the child debug
## handshake to subsequent F5/F6 launches.
func enable_runtime_debug() -> void:
	_runtime_gate_generation += 1
	_runtime_debug_authorized = true
	var session := _session()
	if session == null:
		return
	session.set_runtime_debug_enabled(true)
	bind_session()


## Revoke launch identity, retire the shared proxy connection, then use an
## isolated peer to stop the child endpoint without quitting the game. Hooks
## are released only after retirement is confirmed (or the game is stopped).
## JSON-facing result consumed by the editor MCP service.
func disable_runtime_debug() -> Variant:
	_runtime_gate_generation += 1
	_runtime_debug_authorized = false
	var generation := _runtime_gate_generation
	var session := _session()
	if session == null:
		close()
		return {"error": OK, "fallback_stopped": false, "message": ""}
	session.set_runtime_debug_enabled(false)
	# McpServer.stop() does not cancel a tool already awaiting this connection.
	# Detach it before the retirement RPC so an in-flight screenshot/read cannot
	# share bytes with endpoint shutdown. The reserved RPC uses its own client.
	_retire_proxy_peer()
	var state: Dictionary = session.get_state()
	var run_id := String(state.get("run_id", ""))
	var descriptor_path := String(state.get("descriptor_path", ""))
	var shutdown_error := OK
	var message := ""
	var fallback_stopped := false
	if bool(state.get("running", false)) \
			and not run_id.is_empty() and not descriptor_path.is_empty():
		var tree: SceneTree = service.get_tree() if service != null else null
		var shutdown_peer := _make_shutdown_peer()
		var connect_error: Error = await _connect_shutdown_peer(
				shutdown_peer, run_id, descriptor_path, tree, generation)
		if generation != _runtime_gate_generation:
			shutdown_peer.close()
			return {
				"error": ERR_BUSY,
				"fallback_stopped": false,
				"message": "",
				"stale": true,
			}
		if connect_error == OK:
			var result: McpToolResult = await shutdown_peer.call_tool(
					"game_control",
					{"action": GameMcpTools.INTERNAL_SHUTDOWN_ACTION},
					ENDPOINT_RETIRE_CALL_TIMEOUT_MS)
			if not result.is_error:
				# The child accepted deferred endpoint shutdown. Retire the
				# identity immediately, even if a concurrent re-enable made
				# this continuation stale: that endpoint cannot be resurrected.
				_retire_runtime_identity(session, run_id)
			if generation != _runtime_gate_generation:
				shutdown_peer.close()
				return {
					"error": ERR_BUSY,
					"fallback_stopped": false,
					"message": "",
					"stale": true,
				}
			if result.is_error:
				shutdown_error = ERR_UNAVAILABLE
			else:
				var retired := await _await_descriptor_retired(
						descriptor_path, tree)
				if generation != _runtime_gate_generation:
					shutdown_peer.close()
					return {
						"error": ERR_BUSY,
						"fallback_stopped": false,
						"message": "",
						"stale": true,
					}
				if not retired:
					shutdown_error = ERR_TIMEOUT
		else:
			shutdown_error = connect_error
		shutdown_peer.close()
	if generation != _runtime_gate_generation:
		return {
			"error": ERR_BUSY,
			"fallback_stopped": false,
			"message": "",
			"stale": true,
		}
	if shutdown_error != OK:
		session.poll()
		if not bool(session.get_state().get("running", false)):
			# Nothing was stopped and there is no endpoint left to retire:
			# claiming a safety stop here would tell the user the editor
			# stopped a game that in fact exited on its own.
			message = "Agent server stopped; the managed game had already exited before its runtime debug endpoint could be retired."
			shutdown_error = OK
		elif session.shutdown():
			fallback_stopped = true
			message = "Agent server stopped; the managed game was also stopped because its runtime debug endpoint could not be retired cleanly."
			shutdown_error = OK
		else:
			message = "Agent server stopped, but the runtime debug endpoint could not be retired and the managed game could not be stopped."
	close()
	session.set_runtime_control_hooks(Callable(), Callable())
	return {
		"error": shutdown_error,
		"fallback_stopped": fallback_stopped,
		"message": message,
	}


func _retire_proxy_peer() -> void:
	var retiring_peer := _peer
	_peer = McpPeerClient.new()
	_connected_run_id = ""
	retiring_peer.close()


func current_runtime_needs_relaunch() -> bool:
	if not _runtime_debug_authorized:
		return false
	var session := _session()
	if session == null:
		return false
	var state: Dictionary = session.get_state()
	return bool(state.get("running", false)) \
			and (String(state.get("run_id", "")).is_empty() \
			or String(state.get("descriptor_path", "")).is_empty())


func _retire_runtime_identity(
		session: ShellGameSession,
		run_id: String) -> void:
	if session != null:
		session.retire_runtime_debug_identity(run_id)
	if _runtime_debug_authorized and current_runtime_needs_relaunch() \
			and _game_run != null:
		_game_run.notify_state_changed()


func _make_shutdown_peer() -> McpPeerClient:
	return McpPeerClient.new()


func _connect_shutdown_peer(
	peer: McpPeerClient,
	run_id: String,
	descriptor_path: String,
	tree: SceneTree,
	generation: int
) -> Error:
	var deadline := Time.get_ticks_msec() + DESCRIPTOR_WAIT_MS
	var descriptor: Dictionary = {}
	var session := _session()
	var expected_pid := session.get_pid() if session != null else -1
	while Time.get_ticks_msec() < deadline:
		if generation != _runtime_gate_generation:
			return ERR_BUSY
		descriptor = _read_descriptor(
				descriptor_path, run_id, expected_pid)
		if not descriptor.is_empty():
			break
		if _awaited_run_gone(session, run_id):
			return ERR_UNAVAILABLE
		if tree == null:
			break
		await tree.process_frame
	if descriptor.is_empty():
		_last_error = "The game is running, but its runtime debug endpoint did not become ready."
		return ERR_TIMEOUT
	var remaining := maxi(1, int(deadline - Time.get_ticks_msec()))
	var err: Error = await peer.connect_to_url(
			String(descriptor.get("url", "")), tree, remaining)
	if err != OK:
		_last_error = peer.get_last_error()
	return err


func close() -> void:
	_peer.close()
	_connected_run_id = ""
	_last_error = ""
	_log_cursors.clear()


func forward_runtime_tool(
	run_id: String,
	descriptor_path: String,
	tool: String,
	args: Dictionary
) -> McpToolResult:
	var tree: SceneTree = service.get_tree() if service != null else null
	var err: Error = await _ensure_peer(run_id, descriptor_path, tree)
	if err != OK:
		return McpToolResult.error(_last_error)
	var definition: McpToolDef = GameMcpCatalog.definition(tool)
	var timeout_ms := definition.timeout_ms + PEER_RESPONSE_MARGIN_MS \
			if definition != null else McpPeerClient.DEFAULT_TIMEOUT_MS
	return await _peer.call_tool(tool, args, timeout_ms)


func request_runtime_quit(run_id: String, _descriptor_path: String) -> bool:
	if run_id.is_empty() or run_id != _connected_run_id or not _peer.has_connection():
		return false
	var err := _peer.send_tool_no_wait(
			"game_control", {"action": "quit"})
	# A no-wait request deliberately leaves a response unread. Retire this
	# connection immediately so no later run can consume that stale envelope.
	close()
	return err == OK


func _tool_run_game(args: Dictionary, ctx: McpToolContext) -> Variant:
	var session := _session()
	if session == null:
		return McpToolResult.error("The editor's game session is not ready.")
	var op := String(args.get("op", ""))
	match op:
		"start":
			var mode := String(args.get("mode", "game"))
			var previous_run_id := session.get_current_run_id()
			if not _game_run.start(mode):
				var reason := session.get_last_error()
				if reason.is_empty():
					reason = "Launch failed."
				return McpToolResult.error(reason)
			await _await_restart(session, previous_run_id, ctx.main_tree())
			_invalidate_if_changed(session)
			var state: Dictionary = session.get_state()
			if String(state.get("state", "")) != "running" \
					or (not previous_run_id.is_empty() \
					and String(state.get("run_id", "")) == previous_run_id):
				var restart_reason := session.get_last_error()
				if restart_reason.is_empty():
					restart_reason = "The replacement game process did not become ready."
				return McpToolResult.error(restart_reason)
			var err: Error = await _ensure_peer(
					String(state.get("run_id", "")),
					String(state.get("descriptor_path", "")),
					ctx.main_tree())
			return _managed_state(session, err)
		"stop":
			if not _game_run.stop():
				var stop_reason := session.get_last_error()
				if stop_reason.is_empty():
					stop_reason = "The editor could not stop its game process."
				return McpToolResult.error(stop_reason)
			close()
			return _managed_state(session)
		"state":
			session.poll()
			_invalidate_if_changed(session)
			return _managed_state(session)
		_:
			return McpToolResult.error("op must be start, stop, or state.")


func _proxy_game_state(_args: Dictionary, ctx: McpToolContext) -> Variant:
	var session := _session()
	if session == null:
		return {
			"run": {"state": "unavailable", "running": false},
			"debug": {"connected": false, "error": "The editor game session is not ready."},
		}
	session.poll()
	var state: Dictionary = session.get_state()
	if not bool(state.get("running", false)):
		close()
		return _managed_state(session)
	var remote: McpToolResult = await forward_runtime_tool(
			String(state.get("run_id", "")),
			String(state.get("descriptor_path", "")),
			"game_state",
			{})
	if remote.is_error:
		_last_error = _result_error_text(remote)
		return _managed_state(session, ERR_UNAVAILABLE)
	return {
		"run": state,
		"debug": {"connected": true, "run_id": _connected_run_id},
		"runtime": remote.structured,
	}


func _proxy_game_control(args: Dictionary, ctx: McpToolContext) -> McpToolResult:
	var action: Variant = args.get("action")
	if typeof(action) != TYPE_STRING \
			or not String(action) in GameMcpCatalog.PUBLIC_GAME_CONTROL_ACTIONS:
		return McpToolResult.error(
				"Unknown public game control action '%s'." % String(action))
	return await _proxy_runtime("game_control", args, ctx)


func _proxy_game_entities(args: Dictionary, ctx: McpToolContext) -> McpToolResult:
	return await _proxy_runtime("game_entities", args, ctx)


func _proxy_game_debug(args: Dictionary, ctx: McpToolContext) -> McpToolResult:
	return await _proxy_runtime("game_debug", args, ctx)


func _proxy_game_screenshot(args: Dictionary, ctx: McpToolContext) -> McpToolResult:
	return await _proxy_runtime("game_screenshot", args, ctx)


func _proxy_game_logs(args: Dictionary, ctx: McpToolContext) -> McpToolResult:
	var session_id := String(ctx.args.get("_session_id", ""))
	var forwarded := args.duplicate(true)
	if not forwarded.has("cursor"):
		forwarded["cursor"] = int(_log_cursors.get(session_id, 0))
	var result := await _proxy_runtime("game_logs", forwarded, ctx)
	if not result.is_error and result.structured is Dictionary \
			and result.structured.has("next_cursor"):
		_log_cursors[session_id] = int(result.structured["next_cursor"])
	return result


func _proxy_runtime(
	tool: String,
	args: Dictionary,
	ctx: McpToolContext
) -> McpToolResult:
	var session := _session()
	if session == null:
		return McpToolResult.error(
				"The editor's game session is not ready.")
	session.poll()
	var state: Dictionary = session.get_state()
	if not bool(state.get("running", false)):
		return McpToolResult.error(
				"No editor-launched game is running — call run_game(op=\"start\") first.")
	return await forward_runtime_tool(
			String(state.get("run_id", "")),
			String(state.get("descriptor_path", "")),
			tool,
			args)


func _ensure_peer(
	run_id: String,
	descriptor_path: String,
	tree: SceneTree
) -> Error:
	if run_id.is_empty() or descriptor_path.is_empty():
		_last_error = "The running game has no debug handshake identity."
		return ERR_UNAVAILABLE
	if _connected_run_id == run_id and _peer.has_connection():
		_last_error = ""
		return OK
	close()
	var deadline := Time.get_ticks_msec() + DESCRIPTOR_WAIT_MS
	var descriptor: Dictionary = {}
	var session := _session()
	var expected_pid := session.get_pid() if session != null else -1
	while Time.get_ticks_msec() < deadline:
		descriptor = _read_descriptor(descriptor_path, run_id, expected_pid)
		if not descriptor.is_empty():
			break
		if _awaited_run_gone(session, run_id):
			return ERR_UNAVAILABLE
		if tree == null:
			break
		await tree.process_frame
	if descriptor.is_empty():
		_last_error = "The game is running, but its runtime debug endpoint did not become ready."
		return ERR_TIMEOUT
	var remaining := maxi(1, int(deadline - Time.get_ticks_msec()))
	var err: Error = await _peer.connect_to_url(
			String(descriptor.get("url", "")), tree, remaining)
	if err != OK:
		_last_error = _peer.get_last_error()
		return err
	_connected_run_id = run_id
	_last_error = ""
	return OK


## Both descriptor waits poll this between frames: a wait bound to a run whose
## child died — or was killed and replaced by an F5 relaunch, which leaves the
## session running under a NEW run id while the awaited descriptor can never
## appear — must fail the call now, not pin the caller for the remainder of
## the DESCRIPTOR_WAIT_MS window.
func _awaited_run_gone(session: ShellGameSession, run_id: String) -> bool:
	if session == null:
		return false
	session.poll()
	if not bool(session.get_state().get("running", false)):
		_last_error = "The game process exited before its runtime debug endpoint became ready."
		return true
	if session.get_current_run_id() != run_id:
		_last_error = "The game was relaunched before its previous runtime debug endpoint became ready."
		return true
	return false


func _await_restart(
	session: ShellGameSession,
	previous_run_id: String,
	tree: SceneTree
) -> void:
	if previous_run_id.is_empty() or tree == null:
		return
	var deadline := Time.get_ticks_msec() + 3_000
	while Time.get_ticks_msec() < deadline:
		session.poll()
		var state: Dictionary = session.get_state()
		if String(state.get("state", "")) == "running" \
				and String(state.get("run_id", "")) != previous_run_id:
			return
		await tree.process_frame


func _await_descriptor_retired(path: String, tree: SceneTree) -> bool:
	if path.is_empty() or not FileAccess.file_exists(path):
		return true
	if tree == null:
		return false
	var deadline := Time.get_ticks_msec() + ENDPOINT_SHUTDOWN_WAIT_MS
	while FileAccess.file_exists(path) and Time.get_ticks_msec() < deadline:
		await tree.process_frame
	return not FileAccess.file_exists(path)


func _read_descriptor(
	path: String,
	expected_run_id: String,
	expected_pid: int = -1
) -> Dictionary:
	var absolute := ProjectSettings.globalize_path(path)
	if not FileAccess.file_exists(absolute):
		return {}
	var file := FileAccess.open(absolute, FileAccess.READ)
	if file == null:
		return {}
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if not (parsed is Dictionary):
		return {}
	if String(parsed.get("run_id", "")) != expected_run_id:
		return {}
	if expected_pid > 0 and int(parsed.get("pid", -1)) != expected_pid:
		return {}
	var url := String(parsed.get("url", ""))
	if McpPeerClient._parse_loopback_url(url).is_empty():
		return {}
	return parsed


func _managed_state(
		session: ShellGameSession,
		connect_error: Error = OK) -> Dictionary:
	var state: Dictionary = session.get_state() if session != null else {
				"state": "unavailable",
				"running": false,
			}
	var connected: bool = _connected_run_id == String(state.get("run_id", "")) \
			and _peer.has_connection()
	var debug: Dictionary = {
		"connected": connected,
		"run_id": _connected_run_id if connected else "",
	}
	if connect_error != OK and not _last_error.is_empty():
		debug["error"] = _last_error
	return {"run": state, "debug": debug}


func _invalidate_if_changed(session: ShellGameSession) -> void:
	var run_id := session.get_current_run_id() if session != null else ""
	if run_id != _connected_run_id:
		close()


func _session() -> ShellGameSession:
	return _game_run.session if _game_run != null else null


static func _result_error_text(result: McpToolResult) -> String:
	for block in result.content:
		if block is Dictionary and String(block.get("type", "")) == "text":
			var message := String(block.get("text", "")).strip_edges()
			if not message.is_empty():
				return message
	return "The runtime debug endpoint rejected game_state."
