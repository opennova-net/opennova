class_name GameMcpTools
extends RefCounted

## Curated runtime tool handlers. The adapter fronts MainGame through narrow public
## methods; this module never reaches into its scene or the simulation's
## private state.

## How long game_control quit waits for a running probe to settle first.
const QUIT_PROBE_CANCEL_MS := 2000

var service: GameMcpService
var adapter: GameMcpAdapter


func _init(game_service: GameMcpService, game_adapter: GameMcpAdapter) -> void:
	service = game_service
	adapter = game_adapter


## The catalog name -> handler table. Method references, so a handler that
## goes missing fails at parse time rather than silently dropping its tool.
func _handlers() -> Dictionary:
	return {
		"game_state": _tool_game_state,
		"game_entities": _tool_game_entities,
		"game_render_diagnostics": _tool_game_render_diagnostics,
		"game_capture_bundle": _tool_game_capture_bundle,
		"game_control": _tool_game_control,
		"game_debug": _tool_game_debug,
		"game_menu": _tool_game_menu,
		"game_screenshot": _tool_game_screenshot,
		"game_logs": _tool_game_logs,
		"game_probe": _tool_game_probe,
	}


func register_all(registry: McpToolRegistry) -> void:
	var handlers := _handlers()
	for def in GameMcpCatalog.definitions():
		if not handlers.has(def.name):
			push_error("GameMcpTools has no handler for cataloged tool '%s'." % def.name)
			continue
		registry.register(def, handlers[def.name])


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
			var offset: Variant = McpToolArgs.integer_number(args.get("offset", 0))
			var limit: Variant = McpToolArgs.integer_number(args.get("limit", 64))
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
			var index: Variant = McpToolArgs.integer_number(args.get("index"))
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


func _tool_game_render_diagnostics(
		_args: Dictionary,
		_ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game's renderer diagnostics are unavailable.")
	var value: Variant = adapter.get_mcp_render_diagnostics()
	if not (value is Dictionary) or (value as Dictionary).is_empty():
		return McpToolResult.error(
				"The game has no render world to inspect yet; start or load a mission first.")
	return value


func _tool_game_capture_bundle(
		args: Dictionary,
		ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game's render capture is unavailable.")
	var label: Variant = args.get("label", "render")
	var settle_frames: Variant = McpToolArgs.integer_number(args.get("settle_frames", 2))
	var world_only: Variant = args.get("world_only", true)
	var include_image: Variant = args.get("include_image", true)
	if typeof(label) != TYPE_STRING or String(label).length() > 80 \
			or settle_frames == null or int(settle_frames) < 0 \
			or int(settle_frames) > 180 \
			or typeof(world_only) != TYPE_BOOL \
			or typeof(include_image) != TYPE_BOOL:
		return McpToolResult.error(
				"game_capture_bundle requires label as a string up to 80 characters, "
				+ "settle_frames from 0 to 180, and boolean world_only/include_image.")
	var normalized := {
		"label": String(label),
		"settle_frames": int(settle_frames),
		"world_only": bool(world_only),
		"include_image": bool(include_image),
	}
	var value: Variant = await adapter.capture_mcp_render_bundle(
			normalized, func() -> bool: return ctx.cancelled)
	if ctx.cancelled:
		return McpToolResult.error("Game render capture was cancelled.")
	if not (value is Dictionary):
		return McpToolResult.error("The game returned an invalid render capture bundle.")
	var bundle: Dictionary = value
	if bundle.has("error"):
		return McpToolResult.error(String(bundle["error"]))
	var image_bytes: Variant = bundle.get("image_bytes")
	if not (image_bytes is PackedByteArray) or image_bytes.is_empty():
		return McpToolResult.error("The game render capture returned no lossless PNG bytes.")
	var structured := bundle.duplicate(true)
	structured.erase("image_bytes")
	var safe: Variant = McpJson.sanitize(structured)
	var result := McpToolResult.new()
	if bool(include_image):
		result.add_image(image_bytes, "image/png")
	result.add_text(JSON.stringify(safe, "\t"))
	result.structured = safe
	return result


func _tool_game_control(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var action := String(args.get("action", ""))
	if adapter == null:
		return McpToolResult.error("The game shell cannot be controlled yet.")
	if action == "quit":
		# A probe mid-run must release its restores before the shell tears down.
		var runner := _probe_runner()
		if runner != null and runner.is_running():
			await runner.cancel_and_wait(QUIT_PROBE_CANCEL_MS)
	var result := adapter.mcp_game_control(action)
	if result != OK:
		return McpToolResult.error(
				"Game control '%s' failed: %s." % [
					action, error_string(int(result))])
	var state: Variant = adapter.get_mcp_game_state()
	return state if state is Dictionary else {"ok": true}


func _tool_game_debug(args: Dictionary, _ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game's debug controls are unavailable.")
	var controls := adapter.get_debug_controls()
	if controls == null:
		return McpToolResult.error("The game's debug controls are unavailable.")
	var op := String(args.get("op", ""))
	# State rows answer "can THIS caller write?": the table reports writability
	# for the caller's own confirm_authority, so a confirmed caller is not told
	# its permitted writes are locked and an unconfirmed caller is not shown
	# writable rows whose set/invoke the confirmation precheck will refuse.
	var confirmed := _authority_confirmed(args)
	match op:
		"list":
			# The typed rows (each carrying this caller's live state) reach the
			# wire through their own to_json_value().
			var rows: Array = []
			for row in controls.list_controls(
					StringName(String(args.get("page", ""))),
					String(args.get("filter", "")),
					confirmed):
				rows.append((row as DebugControlRow).to_json_value())
			return {"controls": rows}
		"get":
			var id := StringName(String(args.get("id", "")))
			if id.is_empty():
				return McpToolResult.error("game_debug op=get requires id.")
			return controls.get_state(id, confirmed).to_json_value()
		"set":
			var id := StringName(String(args.get("id", "")))
			if id.is_empty() or not args.has("value"):
				return McpToolResult.error(
						"game_debug op=set requires id and value.")
			if _automation_confirmation_required(controls, id) and not confirmed:
				return McpToolResult.error(_debug_error(id, ERR_UNAUTHORIZED))
			var err: Error = controls.set_value(id, args["value"], confirmed)
			if err != OK:
				return McpToolResult.error(_debug_error(id, err))
			return controls.get_state(id, confirmed).to_json_value()
		"invoke":
			var id := StringName(String(args.get("id", "")))
			if id.is_empty():
				return McpToolResult.error("game_debug op=invoke requires id.")
			if _automation_confirmation_required(controls, id) and not confirmed:
				return McpToolResult.error(_debug_error(id, ERR_UNAUTHORIZED))
			# The row's own arg schema marshals the JSON (by-name object,
			# positional array or one scalar) and names the refused field.
			var marshalled: DebugMarshalResult = controls.marshal_invoke_args(
					id, args.get("args"))
			if marshalled.refused:
				return McpToolResult.error(marshalled.reason)
			var outcome: DebugInvokeResult = controls.invoke(id, marshalled.args, confirmed)
			if outcome.error != OK:
				return McpToolResult.error(_debug_error(id, outcome.error as Error))
			return outcome.to_json_value()
		"snapshot":
			# The table's JSON snapshot plus the shell's runtime block: the
			# op=snapshot wire shape.
			var snapshot: Dictionary = controls.capture_snapshot(
					String(args.get("filter", "")), confirmed)
			snapshot["runtime"] = adapter.runtime_status()
			return snapshot
		_:
			return McpToolResult.error(
					"Unknown game_debug op '%s'." % op)


func _tool_game_menu(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var result: Variant = adapter.mcp_game_menu(args)
	if result is Dictionary and (result as Dictionary).has("error"):
		return McpToolResult.error(String((result as Dictionary)["error"]))
	return result


func _tool_game_screenshot(args: Dictionary, ctx: McpToolContext) -> Variant:
	if adapter == null:
		return McpToolResult.error("The game shell is not ready.")
	return await McpScreenshot.tool_capture(adapter.get_viewport(), args, ctx,
			"game_screenshot", "Game screenshot", "OpenNova game")


func _tool_game_logs(args: Dictionary, ctx: McpToolContext) -> Variant:
	if service == null or service.log_hub == null:
		return McpToolResult.error("Runtime logs are unavailable.")
	var hub: McpLogHub = service.log_hub
	hub.ingest_engine()
	hub.ingest_godot_log()
	var sources := PackedStringArray()
	for source in args.get("sources", []) \
			if args.get("sources") is Array else []:
		sources.append(String(source))
	return hub.session_page(
			service.server.session(String(ctx.args.get("_session_id", ""))),
			int(args.get("cursor", -1)), int(args.get("limit", 200)), sources)


func _tool_game_probe(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var runner := _probe_runner()
	if runner == null:
		return McpToolResult.error("The probe runner is unavailable in this game.")
	var op := String(args.get("op", ""))
	match op:
		"list":
			return runner.list()
		"run":
			if service.server.is_tool_running():
				return McpToolResult.error(
						"A serial tool call is in flight; retry game_probe op=run when it finishes.")
			var name := String(args.get("name", ""))
			if name.is_empty():
				return McpToolResult.error("game_probe op=run requires name.")
			var probe_args: Variant = args.get("args", {})
			if probe_args == null:
				probe_args = {}
			if not (probe_args is Dictionary):
				return McpToolResult.error("game_probe args must be an object.")
			var started: Dictionary = runner.start(name, probe_args)
			if started.has("refused"):
				return McpToolResult.error(String(started["refused"]), started.get("details"))
			return started
		"status":
			var cursor: Variant = McpToolArgs.integer_number(args.get("cursor", 0))
			var wait_ms: Variant = McpToolArgs.integer_number(args.get("wait_ms", 0))
			if cursor == null or int(cursor) < 0 or wait_ms == null or int(wait_ms) < 0 \
					or int(wait_ms) > GameMcpCatalog.PROBE_STATUS_WAIT_MAX_MS:
				return McpToolResult.error(
						"game_probe op=status requires cursor >= 0 and wait_ms from 0 to %d." % [
							GameMcpCatalog.PROBE_STATUS_WAIT_MAX_MS])
			var status: Dictionary = await runner.status(
					String(args.get("run_id", "")), int(cursor), int(wait_ms))
			if status.has("refused"):
				return McpToolResult.error(String(status["refused"]))
			return status
		"cancel":
			var outcome: Dictionary = runner.cancel(String(args.get("run_id", "")))
			if outcome.has("refused"):
				return McpToolResult.error(String(outcome["refused"]))
			return outcome
		_:
			return McpToolResult.error("Unknown game_probe op '%s'." % op)


func _probe_runner() -> ProbeRunner:
	return service.probe_runner if service != null else null


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


static func _authority_confirmed(args: Dictionary) -> bool:
	var value: Variant = args.get("confirm_authority", false)
	return typeof(value) == TYPE_BOOL and bool(value)


## The op=set/invoke precheck: a confirmation-gated control is refused for an
## unconfirmed caller before its JSON args are even marshalled. The table's
## own policy repeats the refusal (and reports it in state rows); the
## precheck keeps the boundary's refusal order stable.
static func _automation_confirmation_required(
		controls: DebugControlTable,
		id: StringName) -> bool:
	if controls == null:
		return false
	var row: DebugControlRow = controls.control(id)
	return row != null and (
			row.requires_confirm
			or row.authority == DebugControlRow.HOST_ONLY)
