class_name GameMcpTools
extends RefCounted

## Curated runtime tool handlers. The adapter fronts MainGame through narrow public
## methods; this module never reaches into its scene or the simulation's
## private state. ONED registers the same definitions with proxy handlers.

const INTERNAL_SHUTDOWN_ACTION := "_oned_shutdown_runtime_debug"

var service: Node
var adapter: GameMcpAdapter
var _endpoint_shutdown := Callable()


func _init(
		game_service: Node,
		game_adapter: GameMcpAdapter,
		endpoint_shutdown: Callable = Callable()) -> void:
	service = game_service
	adapter = game_adapter
	_endpoint_shutdown = endpoint_shutdown
	if not _endpoint_shutdown.is_valid() and service != null:
		_endpoint_shutdown = Callable(service, "request_endpoint_shutdown")


func register_all(registry: McpToolRegistry) -> void:
	for def in GameMcpCatalog.definitions():
		var handler := Callable(self, "_tool_%s" % def.name)
		if handler.is_valid():
			registry.register(def, handler)


func _tool_game_state(_args: Dictionary, _ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game shell is not ready.")
	var state: Variant = adapter.get_mcp_game_state()
	return state if state is Dictionary else McpToolResult.error(
			"The game shell returned an invalid state snapshot.")


func _tool_game_entities(args: Dictionary, _ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game's entity diagnostics are unavailable.")
	var op := String(args.get("op", ""))
	match op:
		"list":
			var offset: Variant = _integer_number(args.get("offset", 0))
			var limit: Variant = _integer_number(args.get("limit", 64))
			if offset == null or int(offset) < 0 \
					or limit == null or int(limit) < 1 or int(limit) > 128:
				return McpToolResult.error(
						"game_entities op=list requires offset >= 0 and limit from 1 to 128.")
			var page_value: Variant = adapter.get_mcp_game_entities(
					int(offset), int(limit))
			if not (page_value is Dictionary):
				return McpToolResult.error(
						"The game returned an invalid entity page.")
			var page: Dictionary = page_value
			if page.is_empty():
				return McpToolResult.error(
						"Start a playable mission before listing entities.")
			return page
		"inspect":
			var index: Variant = _integer_number(args.get("index"))
			if index == null or int(index) < 0:
				return McpToolResult.error(
						"game_entities op=inspect requires a non-negative integer index.")
			var entity_value: Variant = adapter.get_mcp_game_entity(int(index))
			if not (entity_value is Dictionary):
				return McpToolResult.error(
						"The game returned invalid entity diagnostics.")
			var entity: Dictionary = entity_value
			if entity.is_empty():
				return McpToolResult.error(
						"Entity index %d is unavailable." % int(index))
			return {"entity": entity}
		_:
			return McpToolResult.error(
					"Unknown game_entities op '%s'." % op)


func _tool_game_control(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var action := String(args.get("action", ""))
	if action == INTERNAL_SHUTDOWN_ACTION:
		if not _endpoint_shutdown.is_valid():
			return McpToolResult.error(
					"The runtime debug endpoint cannot shut down cleanly.")
		_endpoint_shutdown.call()
		return {"ok": true, "debug_endpoint": "stopping"}
	if adapter == null:
		return McpToolResult.error("The game shell cannot be controlled yet.")
	var result := adapter.mcp_game_control(action)
	if result != OK:
		return McpToolResult.error(
				"Game control '%s' failed: %s." % [
					action, error_string(int(result))])
	var state: Variant = adapter.get_mcp_game_state()
	return state if state is Dictionary else {"ok": true}


func _tool_game_debug(args: Dictionary, _ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game's debug session is unavailable.")
	var session := adapter.get_debug_session()
	if session == null:
		return McpToolResult.error("The game's debug session is unavailable.")
	var op := String(args.get("op", ""))
	# State rows answer "can THIS caller write?" in both directions: a
	# confirm_authority caller is not told its permitted writes are locked
	# behind F3's latch, and an unconfirmed caller is not shown writable rows
	# whose set/invoke the confirm_authority precheck will refuse.
	var confirmed := _authority_confirmed(args)
	match op:
		"list":
			var rows: Array[Dictionary] = session.list_controls(
					StringName(String(args.get("page", ""))),
					String(args.get("filter", "")),
					confirmed)
			for row in rows:
				_apply_confirmation_gate(session,
						StringName(String(row.get("id", ""))),
						confirmed, row.get("state"))
			return {"controls": rows}
		"get":
			var id := StringName(String(args.get("id", "")))
			if id.is_empty():
				return McpToolResult.error("game_debug op=get requires id.")
			return _apply_confirmation_gate(session, id, confirmed,
					session.get_control_state(id, confirmed).to_json_value())
		"set":
			var id := StringName(String(args.get("id", "")))
			if id.is_empty() or not args.has("value"):
				return McpToolResult.error(
						"game_debug op=set requires id and value.")
			if _automation_confirmation_required(session, id) and not confirmed:
				return McpToolResult.error(_debug_error(id, ERR_UNAUTHORIZED))
			var err: Error = session.set_control_value(
					id, args["value"], confirmed)
			if err != OK:
				return McpToolResult.error(_debug_error(id, err))
			return session.get_control_state(id, confirmed).to_json_value()
		"invoke":
			var id := StringName(String(args.get("id", "")))
			if id.is_empty():
				return McpToolResult.error("game_debug op=invoke requires id.")
			if _automation_confirmation_required(session, id) and not confirmed:
				return McpToolResult.error(_debug_error(id, ERR_UNAUTHORIZED))
			var call_args: Variant = _debug_action_args(id, args.get("args"))
			if call_args is McpToolResult:
				return call_args
			var outcome: Variant = session.invoke_control(
					id,
					call_args,
					confirmed)
			if not (outcome is Dictionary):
				return McpToolResult.error(
						"Debug control '%s' returned an invalid result." % id)
			var err := int(outcome.get("error", FAILED))
			if err != OK:
				return McpToolResult.error(_debug_error(id, err))
			return outcome
		"snapshot":
			var snapshot: Dictionary = session.capture_snapshot(
					String(args.get("filter", "")), confirmed)
			if snapshot.is_empty():
				return McpToolResult.error(
						"Start a playable mission before capturing a debug snapshot.")
			for row in snapshot.get("controls", []):
				if row is Dictionary:
					_apply_confirmation_gate(session,
							StringName(String(row.get("id", ""))),
							confirmed, row.get("state"))
			return snapshot
		_:
			return McpToolResult.error(
					"Unknown game_debug op '%s'." % op)


func _tool_game_screenshot(args: Dictionary, ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game shell is not ready.")
	# Validate argument types before acquiring the presentation lease: a script
	# error between acquire and release would leak the lease and pin expensive
	# debug views on for the rest of the run.
	var max_dim: Variant = _integer_number(
			args.get("max_dim", McpScreenshot.DEFAULT_MAX_DIM))
	var format: Variant = args.get("format", "webp")
	var quality: Variant = _finite_number(
			args.get("quality", McpScreenshot.DEFAULT_QUALITY))
	if max_dim == null or typeof(format) != TYPE_STRING or quality == null:
		return McpToolResult.error(
				"game_screenshot requires an integer max_dim, a string format, "
				+ "and a numeric quality.")
	var debug_session := adapter.get_debug_session()
	if debug_session != null:
		debug_session.acquire_presentation_source(&"mcp_screenshot")
	var viewport := adapter.get_viewport()
	var outcome: Dictionary = await McpScreenshot.capture(viewport, {
		"max_dim": int(max_dim),
		"format": String(format),
		"quality": float(quality),
	}, func() -> bool: return ctx.cancelled)
	if debug_session != null:
		debug_session.release_presentation_source(&"mcp_screenshot")
	if ctx.cancelled:
		return McpToolResult.error(
				"Game screenshot was cancelled after its request timed out.")
	if not bool(outcome.get("ok", false)):
		return McpToolResult.error(String(outcome.get(
				"error", "Game screenshot failed.")))
	var caption := "OpenNova game (%dx%d)" % [
		int(outcome["width"]), int(outcome["height"])]
	if outcome.has("warning"):
		caption += " — " + String(outcome["warning"])
	return McpToolResult.image(outcome["bytes"], outcome["mime"], caption)


func _tool_game_logs(args: Dictionary, ctx: McpToolContext) -> Variant:
	if service == null or service.get("log_hub") == null:
		return McpToolResult.error("Runtime logs are unavailable.")
	var hub: McpLogHub = service.log_hub
	hub.ingest_engine()
	var session: Dictionary = service.server.session(
			String(ctx.args.get("_session_id", "")))
	var cursor := int(args.get("cursor", -1))
	if cursor < 0:
		cursor = int(session.get("log_cursor", 0)) \
				if not session.is_empty() else 0
	var sources := PackedStringArray()
	for source in args.get("sources", []) \
			if args.get("sources") is Array else []:
		sources.append(String(source))
	var page := hub.get_entries(
			cursor, int(args.get("limit", 200)), sources)
	if not session.is_empty():
		session["log_cursor"] = page["next_cursor"]
	return page


static func _debug_error(id: StringName, err: Error) -> String:
	match err:
		ERR_DOES_NOT_EXIST:
			return "Unknown debug control '%s'." % id
		ERR_UNAVAILABLE:
			return "Debug control '%s' is unavailable in the current game state." % id
		ERR_UNAUTHORIZED:
			return "Debug control '%s' changes authoritative state; pass confirm_authority=true on an authority-owning session." % id
		ERR_INVALID_PARAMETER:
			return "The value or arguments for debug control '%s' are invalid." % id
		_:
			return "Debug control '%s' failed: %s." % [id, error_string(err)]


static func _debug_action_args(id: StringName, raw: Variant) -> Variant:
	if raw is Array:
		if id in [
			&"teleport_local_player",
			&"set_entity_health",
			&"set_entity_position",
			&"runtime_transport",
			&"set_mission_variable",
			&"set_audio_bus_volume",
			&"set_audio_bus_mute",
			&"set_audio_bus_solo",
			&"set_audio_bus_bypass",
		]:
			return McpToolResult.error(
					"Debug action '%s' requires its documented args object." % id)
		return raw
	if raw == null:
		return null
	if not (raw is Dictionary):
		return raw
	var args: Dictionary = raw
	match id:
		&"teleport_local_player":
			var position: Variant = _vector3_arg(args.get("position"))
			var yaw: Variant = _finite_number(args.get("yaw_deg", 0.0))
			var pitch: Variant = _finite_number(args.get("pitch_deg", 0.0))
			if position == null or yaw == null or pitch == null:
				return McpToolResult.error(
						"teleport_local_player requires numeric position, yaw_deg, and pitch_deg.")
			return [position, yaw, pitch]
		&"set_entity_health":
			if not args.has("entity") or not args.has("health"):
				return McpToolResult.error(
						"set_entity_health requires args.entity and args.health.")
			var entity: Variant = _integer_number(args["entity"])
			var health: Variant = _integer_number(args["health"])
			if entity == null or health == null:
				return McpToolResult.error(
						"set_entity_health requires integer entity and health values.")
			return [entity, health]
		&"set_entity_position":
			var position: Variant = _vector3_arg(args.get("position"))
			var entity: Variant = _integer_number(args.get("entity"))
			if entity == null or position == null:
				return McpToolResult.error(
						"set_entity_position requires an integer entity and numeric position=[x,y,z].")
			return [entity, position]
		&"runtime_transport":
			var action: Variant = args.get("action")
			if typeof(action) != TYPE_STRING or String(action).is_empty():
				return McpToolResult.error(
						"runtime_transport requires a string args.action.")
			return action
		&"set_mission_variable":
			if not args.has("index") or not args.has("value"):
				return McpToolResult.error(
						"set_mission_variable requires args.index and args.value.")
			var index: Variant = _integer_number(args["index"])
			var value: Variant = _integer_number(args["value"])
			if index == null or value == null:
				return McpToolResult.error(
						"set_mission_variable requires integer index and value fields.")
			return [index, value]
		&"set_audio_bus_volume":
			var bus: Variant = _audio_bus_name(args)
			var volume_db: Variant = _finite_number(args.get("volume_db"))
			if bus == null or volume_db == null:
				return McpToolResult.error(
						"set_audio_bus_volume requires a non-empty string bus and numeric volume_db.")
			return [bus, volume_db]
		&"set_audio_bus_mute":
			return _audio_bus_switch_args(
					args, "muted", "set_audio_bus_mute")
		&"set_audio_bus_solo":
			return _audio_bus_switch_args(
					args, "soloed", "set_audio_bus_solo")
		&"set_audio_bus_bypass":
			return _audio_bus_switch_args(
					args, "bypassed", "set_audio_bus_bypass")
		_:
			return args.get("values", null)


static func _vector3_arg(value: Variant) -> Variant:
	if value is Vector3:
		return value if value.is_finite() else null
	if value is Array and value.size() == 3:
		var x: Variant = _finite_number(value[0])
		var y: Variant = _finite_number(value[1])
		var z: Variant = _finite_number(value[2])
		return Vector3(x, y, z) \
				if x != null and y != null and z != null else null
	if value is Dictionary and value.has("x") and value.has("y") and value.has("z"):
		var x: Variant = _finite_number(value["x"])
		var y: Variant = _finite_number(value["y"])
		var z: Variant = _finite_number(value["z"])
		return Vector3(x, y, z) \
				if x != null and y != null and z != null else null
	return null


static func _authority_confirmed(args: Dictionary) -> bool:
	var value: Variant = args.get("confirm_authority", false)
	return typeof(value) == TYPE_BOOL and bool(value)


static func _automation_confirmation_required(
		session: NovaDebugSession,
		id: StringName) -> bool:
	if session == null:
		return false
	var definition: NovaDebugControlDef = session.definition(id)
	return definition != null and (
			definition.requires_unlock
			or definition.authority == NovaDebugControlDef.Authority.HOST_ONLY)


## Mirror the op=set/invoke confirm_authority precheck in reported rows: for
## an unconfirmed caller a confirmation-gated control is not writable no
## matter what F3's Live-edits latch says, because that caller's write would
## be refused before reaching the session.
static func _apply_confirmation_gate(
		session: NovaDebugSession,
		id: StringName,
		confirmed: bool,
		state: Variant) -> Variant:
	if confirmed or not (state is Dictionary) \
			or not _automation_confirmation_required(session, id):
		return state
	if bool(state.get("writable", false)):
		state["writable"] = false
		state["reason"] = "This control changes authoritative state; pass confirm_authority=true on an authority-owning session."
	return state


static func _finite_number(value: Variant) -> Variant:
	if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
		return null
	var number := float(value)
	return number if is_finite(number) else null


static func _integer_number(value: Variant) -> Variant:
	var number: Variant = _finite_number(value)
	if number == null or float(number) != floorf(float(number)):
		return null
	return int(number)


static func _audio_bus_name(args: Dictionary) -> Variant:
	var bus: Variant = args.get("bus")
	if typeof(bus) != TYPE_STRING or String(bus).is_empty():
		return null
	return String(bus)


static func _audio_bus_switch_args(
		args: Dictionary,
		field: String,
		action: String) -> Variant:
	var bus: Variant = _audio_bus_name(args)
	var enabled: Variant = args.get(field)
	if bus == null or typeof(enabled) != TYPE_BOOL:
		return McpToolResult.error(
				"%s requires a non-empty string bus and boolean %s." % [
					action, field])
	return [bus, enabled]
