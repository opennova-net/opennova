extends GutTest


class LogProxy:
	extends EditorMcpGameTools

	var forwarded: Array[Dictionary] = []

	func _init() -> void:
		super(null)

	func _proxy_runtime(
			tool: String,
			args: Dictionary,
			_ctx: McpToolContext) -> McpToolResult:
		assert(tool == "game_logs")
		forwarded.append(args.duplicate(true))
		return McpToolResult.json({
			"entries": [],
			"next_cursor": int(args.get("cursor", 0)) + 1,
		})


class ControlProxy:
	extends EditorMcpGameTools

	var forwarded := false

	func _init() -> void:
		super(null)

	func _proxy_runtime(
			_tool: String,
			_args: Dictionary,
			_ctx: McpToolContext) -> McpToolResult:
		forwarded = true
		return McpToolResult.json({"ok": true})


class QuitPeer:
	extends McpPeerClient

	var sent := false
	var closed := false

	func has_connection() -> bool:
		return true

	func send_tool_no_wait(name: String, args := {}) -> Error:
		sent = name == "game_control" \
				and args is Dictionary and args.get("action") == "quit"
		return OK

	func close() -> void:
		closed = true


class RuntimeGateSession:
	extends ShellGameSession

	var events: Array
	var debug_enabled := true
	var hooks_bound := true
	var running := true
	var shutdown_result := true
	var last_error := ""
	var run_id := "run-17"
	var descriptor_path := "user://missing-runtime-gate-descriptor.json"

	func _init(shared_events: Array) -> void:
		events = shared_events

	func set_runtime_debug_enabled(value: bool) -> void:
		debug_enabled = value
		events.append("identity_disabled" if not value else "identity_enabled")

	func set_runtime_control_hooks(
			forwarder: Callable,
			quit_requester: Callable = Callable()) -> void:
		hooks_bound = forwarder.is_valid() or quit_requester.is_valid()
		if not hooks_bound:
			events.append("hooks_unbound")

	func get_state() -> Dictionary:
		return {
			"state": "running",
			"running": running,
			"pid": 4017,
			"run_id": run_id,
			"descriptor_path": descriptor_path,
		}

	func get_pid() -> int:
		return 4017

	func get_last_error() -> String:
		return last_error

	func retire_runtime_debug_identity(expected_run_id: String) -> bool:
		if run_id != expected_run_id:
			return false
		run_id = ""
		descriptor_path = ""
		events.append("identity_retired")
		return true

	func shutdown() -> bool:
		events.append("game_stopped_for_safety")
		if shutdown_result:
			running = false
		return shutdown_result


class RuntimeGateShell:
	extends Node

	var session: RuntimeGateSession
	var stop_result := true

	func _init(value: RuntimeGateSession) -> void:
		session = value

	func get_game_run_session() -> Variant:
		return session

	func stop_game() -> bool:
		return stop_result


class RuntimeGateService:
	extends Node

	var shell: RuntimeGateShell
	var runtime_status_changes := 0

	func _init(value: RuntimeGateShell) -> void:
		shell = value

	func runtime_debug_state_changed() -> void:
		runtime_status_changes += 1


class ShutdownPeer:
	extends McpPeerClient

	var events: Array
	var action := ""
	var call_timeout_ms := 0
	var close_event := "peer_closed"

	func _init(shared_events: Array, event_name := "peer_closed") -> void:
		events = shared_events
		close_event = event_name

	func has_connection() -> bool:
		return true

	func call_tool(
			name: String,
			args := {},
			timeout_ms := DEFAULT_TIMEOUT_MS) -> McpToolResult:
		call_timeout_ms = timeout_ms
		action = String(args.get("action", "")) if args is Dictionary else ""
		if name == "game_control":
			events.append("endpoint_shutdown")
		return McpToolResult.json({"ok": true})

	func close() -> void:
		events.append(close_event)


class ConnectOnDisableProxy:
	extends EditorMcpGameTools

	var ensured_without_existing_peer := false
	var shutdown_peer: McpPeerClient

	func _make_shutdown_peer() -> McpPeerClient:
		return shutdown_peer

	func _connect_shutdown_peer(
			_peer: McpPeerClient,
			run_id: String,
			_descriptor_path: String,
			_tree: SceneTree,
			_generation: int) -> Error:
		ensured_without_existing_peer = true
		assert(run_id == "run-17")
		return OK


class DelayedShutdownPeer:
	extends ShutdownPeer

	signal release_response

	var started := false

	func call_tool(
			name: String,
			args := {},
			_timeout_ms := DEFAULT_TIMEOUT_MS) -> McpToolResult:
		action = String(args.get("action", "")) if args is Dictionary else ""
		if name == "game_control":
			events.append("endpoint_shutdown_started")
		started = true
		await release_response
		events.append("endpoint_shutdown_finished")
		return McpToolResult.json({"ok": true})


class FailingShutdownPeer:
	extends ShutdownPeer

	func call_tool(
			name: String,
			args := {},
			_timeout_ms := DEFAULT_TIMEOUT_MS) -> McpToolResult:
		action = String(args.get("action", "")) if args is Dictionary else ""
		if name == "game_control":
			events.append("endpoint_shutdown_failed")
		return McpToolResult.error("expected endpoint failure")


class DyingChildSession:
	extends RuntimeGateSession

	var polls := 0

	func _init(shared_events: Array) -> void:
		super(shared_events)

	func poll() -> void:
		polls += 1
		running = false


class RelaunchedChildSession:
	extends RuntimeGateSession

	func _init(shared_events: Array) -> void:
		super(shared_events)
		run_id = "run-18"

	func poll() -> void:
		pass

	func get_current_run_id() -> String:
		return run_id


class ExitedChildDisableProxy:
	extends ConnectOnDisableProxy

	func _connect_shutdown_peer(
			_peer: McpPeerClient,
			_run_id: String,
			_descriptor_path: String,
			_tree: SceneTree,
			_generation: int) -> Error:
		return ERR_UNAVAILABLE


func _context(session_id: String) -> McpToolContext:
	var ctx := McpToolContext.new()
	ctx.args = {"_session_id": session_id}
	return ctx


func _call(
		proxy: EditorMcpGameTools,
		name: String,
		args: Dictionary,
		ctx: McpToolContext) -> McpToolResult:
	var registry := McpToolRegistry.new()
	proxy.register_all(registry)
	return await registry.call_tool(name, args, ctx)


func test_game_log_cursor_is_independent_for_each_editor_client() -> void:
	var proxy := LogProxy.new()
	await _call(proxy, "game_logs", {}, _context("client-a"))
	await _call(proxy, "game_logs", {}, _context("client-b"))
	await _call(proxy, "game_logs", {}, _context("client-a"))

	assert_eq(proxy.forwarded[0]["cursor"], 0)
	assert_eq(proxy.forwarded[1]["cursor"], 0)
	assert_eq(proxy.forwarded[2]["cursor"], 1)


func test_public_proxy_cannot_retire_the_private_runtime_endpoint() -> void:
	var proxy := ControlProxy.new()
	var result := await _call(proxy, "game_control", {
		"action": GameMcpTools.INTERNAL_SHUTDOWN_ACTION,
	}, _context("client-a"))

	assert_true(result.is_error)
	assert_false(proxy.forwarded,
			"the private retirement action never crosses the public proxy")

	result = await _call(proxy, "game_control",
			{"action": "pause"}, _context("client-a"))
	assert_false(result.is_error)
	assert_true(proxy.forwarded)


func test_graceful_quit_retires_the_no_wait_peer() -> void:
	var peer := QuitPeer.new()
	var proxy := EditorMcpGameTools.new(null, null, peer, "run-17")

	assert_true(proxy.request_runtime_quit("run-17", "ignored"))
	assert_true(peer.sent)
	assert_true(peer.closed)
	assert_false(proxy.request_runtime_quit("run-17", "ignored"),
			"the no-wait response cannot leak into a later request")


func test_descriptor_wait_fails_fast_when_the_child_process_dies() -> void:
	var events: Array = []
	var session := DyingChildSession.new(events)
	var service: Node = add_child_autofree(Node.new())
	var bridge := EditorGameRunBridge.new(session)
	var proxy := EditorMcpGameTools.new(service, bridge)

	var started_ms := Time.get_ticks_msec()
	var result: McpToolResult = await proxy.forward_runtime_tool(
			"run-17", "user://missing-runtime-gate-descriptor.json",
			"game_state", {})

	assert_true(result.is_error)
	assert_string_contains(String(result.content[0]["text"]), "exited")
	assert_gt(session.polls, 0, "the wait loop re-polls child liveness")
	assert_lt(int(Time.get_ticks_msec() - started_ms), 5_000,
			"a dead child cannot pin the caller for the full descriptor window")


func test_descriptor_wait_fails_fast_when_the_awaited_run_is_replaced() -> void:
	# An F5 relaunch mid-wait leaves the session running under a NEW run id;
	# the awaited descriptor can never appear, so waiting out the window would
	# stall and then blame the healthy replacement run.
	var events: Array = []
	var session := RelaunchedChildSession.new(events)
	var service: Node = add_child_autofree(Node.new())
	var bridge := EditorGameRunBridge.new(session)
	var proxy := EditorMcpGameTools.new(service, bridge)

	var started_ms := Time.get_ticks_msec()
	var result: McpToolResult = await proxy.forward_runtime_tool(
			"run-17", "user://missing-runtime-gate-descriptor.json",
			"game_state", {})

	assert_true(result.is_error)
	assert_string_contains(String(result.content[0]["text"]), "relaunched")
	assert_lt(int(Time.get_ticks_msec() - started_ms), 5_000,
			"a replaced run cannot pin the caller for the full descriptor window")


func test_disable_reports_an_already_exited_child_truthfully() -> void:
	# When the child died before publishing its descriptor, nothing is stopped
	# in the fallback — the result must not claim a safety stop the editor
	# never performed.
	var events: Array = []
	var session := DyingChildSession.new(events)
	var shell: RuntimeGateShell = add_child_autofree(
			RuntimeGateShell.new(session))
	var service: RuntimeGateService = add_child_autofree(
			RuntimeGateService.new(shell))
	var proxy_peer := ShutdownPeer.new(events, "proxy_peer_retired")
	var bridge := EditorGameRunBridge.new(session)
	var proxy := ExitedChildDisableProxy.new(service, bridge, proxy_peer)
	proxy.shutdown_peer = ShutdownPeer.new(events, "shutdown_peer_closed")

	var outcome: Dictionary = await proxy.disable_runtime_debug()

	assert_eq(outcome["error"], OK)
	assert_false(outcome["fallback_stopped"],
			"a crash is not reported as an editor-initiated stop")
	assert_string_contains(String(outcome["message"]), "already exited")
	assert_false(events.has("game_stopped_for_safety"),
			"no shutdown is issued against an already-stopped session")


func test_mcp_stop_reports_managed_process_failure() -> void:
	var events: Array = []
	var session := RuntimeGateSession.new(events)
	session.last_error = "Could not stop the running game."
	var shell: RuntimeGateShell = add_child_autofree(
			RuntimeGateShell.new(session))
	shell.stop_result = false
	var service: RuntimeGateService = add_child_autofree(
			RuntimeGateService.new(shell))
	var bridge := EditorGameRunBridge.new(
			session,
			Callable(),
			func() -> bool: return shell.stop_game())
	var proxy := EditorMcpGameTools.new(service, bridge)
	var ctx := McpToolContext.new()
	ctx.shell = shell

	var result := await _call(proxy, "run_game", {"op": "stop"}, ctx)

	assert_true(result.is_error)
	assert_string_contains(
			String(result.content[0]["text"]),
			"Could not stop")


func test_disabling_runtime_debug_retires_child_before_peer_and_hooks() -> void:
	var events: Array = []
	var session := RuntimeGateSession.new(events)
	var shell: RuntimeGateShell = add_child_autofree(
			RuntimeGateShell.new(session))
	var service: RuntimeGateService = add_child_autofree(
			RuntimeGateService.new(shell))
	var proxy_peer := ShutdownPeer.new(events, "proxy_peer_retired")
	var shutdown_peer := ShutdownPeer.new(events, "shutdown_peer_closed")
	var bridge := EditorGameRunBridge.new(session)
	var proxy := ConnectOnDisableProxy.new(service, bridge, proxy_peer)
	proxy.shutdown_peer = shutdown_peer

	var outcome: Dictionary = await proxy.disable_runtime_debug()

	assert_eq(outcome["error"], OK)
	assert_eq(events, [
		"identity_disabled",
		"proxy_peer_retired",
		"endpoint_shutdown",
		"identity_retired",
		"shutdown_peer_closed",
		"hooks_unbound",
	])
	assert_true(proxy.ensured_without_existing_peer,
			"disable connects from the descriptor when no proxy peer existed")
	assert_eq(shutdown_peer.action, GameMcpTools.INTERNAL_SHUTDOWN_ACTION)
	assert_eq(shutdown_peer.call_timeout_ms,
			EditorMcpGameTools.ENDPOINT_RETIRE_CALL_TIMEOUT_MS)
	assert_gt(shutdown_peer.call_timeout_ms, GameMcpCatalog.SCREENSHOT_TIMEOUT_MS,
			"endpoint retirement drains any already-running screenshot first")
	assert_false(session.debug_enabled)
	assert_false(session.hooks_bound)
	assert_true(session.get_state()["running"],
			"disabling debug leaves the managed game process alone")


func test_reenable_marks_current_game_for_relaunch_after_endpoint_retires() -> void:
	var events: Array = []
	var session := RuntimeGateSession.new(events)
	var shell: RuntimeGateShell = add_child_autofree(
			RuntimeGateShell.new(session))
	var service: RuntimeGateService = add_child_autofree(
			RuntimeGateService.new(shell))
	var proxy_peer := ShutdownPeer.new(events, "proxy_peer_retired")
	var peer := DelayedShutdownPeer.new(events, "shutdown_peer_closed")
	var bridge := EditorGameRunBridge.new(
			session,
			Callable(),
			Callable(),
			func() -> void: service.runtime_debug_state_changed())
	var proxy := ConnectOnDisableProxy.new(service, bridge, proxy_peer)
	proxy.shutdown_peer = peer

	proxy.disable_runtime_debug()
	await get_tree().process_frame
	assert_true(peer.started)
	proxy.enable_runtime_debug()
	peer.release_response.emit()
	await get_tree().process_frame
	await get_tree().process_frame

	assert_true(session.debug_enabled)
	assert_true(session.hooks_bound)
	assert_eq(session.run_id, "",
			"the accepted shutdown cannot leave a live-looking handshake")
	assert_true(proxy.current_runtime_needs_relaunch())
	assert_eq(service.runtime_status_changes, 1,
			"Settings is told the current runtime needs relaunching")
	assert_false(events.has("hooks_unbound"),
			"a stale disable continuation cannot unbind re-enabled hooks")
	assert_eq(events.count("proxy_peer_retired"), 1,
			"only the pre-enable proxy peer is retired")


func test_failed_endpoint_retirement_stops_managed_game_for_safety() -> void:
	var events: Array = []
	var session := RuntimeGateSession.new(events)
	var shell: RuntimeGateShell = add_child_autofree(
			RuntimeGateShell.new(session))
	var service: RuntimeGateService = add_child_autofree(
			RuntimeGateService.new(shell))
	var proxy_peer := ShutdownPeer.new(events, "proxy_peer_retired")
	var peer := FailingShutdownPeer.new(events, "shutdown_peer_closed")
	var bridge := EditorGameRunBridge.new(session)
	var proxy := ConnectOnDisableProxy.new(service, bridge, proxy_peer)
	proxy.shutdown_peer = peer

	var outcome: Dictionary = await proxy.disable_runtime_debug()

	assert_eq(outcome["error"], OK)
	assert_true(outcome["fallback_stopped"])
	assert_string_contains(String(outcome["message"]), "game was also stopped")
	assert_false(session.running)
	assert_lt(events.find("endpoint_shutdown_failed"),
			events.find("game_stopped_for_safety"))
	assert_lt(events.find("game_stopped_for_safety"),
			events.find("hooks_unbound"))
