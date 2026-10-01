class_name EditorMcpTools
extends RefCounted

## The editor MCP's handlers over the editor's wire seam (EditorApp.request_json and query_json,
## ADR 0046 S13 A5): a request and a query cross as JSON text the portable session marshals, so
## this module forwards and waits; it never reaches into the session, and it pages nothing (the
## queries page their lists). The two preview tools stay device tools until S13 V7, each answering
## with its viewport's envelope (S13 V5, editor/preview/viewport_json.h).

## How long editor_play op=stop waits for the game to leave.
const STOP_WAIT_MS := 10_000

var service: EditorMcpService
var app: Node


func _init(editor_service: EditorMcpService, editor_app: Node) -> void:
	service = editor_service
	app = editor_app


## The catalog name -> handler table. Method references, so a handler that goes
## missing fails at parse time rather than silently dropping its tool.
func _handlers() -> Dictionary:
	return {
		"editor_state": _tool_editor_state,
		"editor_request": _tool_editor_request,
		"editor_query": _tool_editor_query,
		"editor_build": _tool_editor_build,
		"editor_play": _tool_editor_play,
		"editor_menu_preview": _tool_editor_menu_preview,
		"editor_model_preview": _tool_editor_model_preview,
		"editor_screenshot": _tool_editor_screenshot,
		"editor_logs": _tool_editor_logs,
	}


func register_all(registry: McpToolRegistry) -> void:
	var handlers := _handlers()
	for def in EditorMcpCatalog.definitions(app):
		if not handlers.has(def.name):
			push_error("EditorMcpTools has no handler for cataloged tool '%s'." % def.name)
			continue
		registry.register(def, handlers[def.name])


func _tool_editor_state(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var query := args.duplicate()
	query.erase("_session_id")
	return _answer("state", query)


func _tool_editor_query(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var params := args.duplicate()
	params.erase("_session_id")
	var name := String(params.get("query", ""))
	params.erase("query")
	return _answer(name, params)


func _tool_editor_request(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var request := args.duplicate()
	request.erase("_session_id")
	# Unsorted: a replace_list record's fields are set in the order written, and JSON.stringify
	# sorts a Dictionary's keys unless told not to.
	var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request, "", false))))
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if not bool(answer.get("ok", false)):
		return McpToolResult.error(String(answer.get("error", "The request was refused.")))
	return answer


## The build raised as the windows raise it, then frames awaited until its operation ends: the
## editor keeps drawing and answering the while. A build running already is joined, not started
## again.
func _tool_editor_build(_args: Dictionary, ctx: McpToolContext) -> Variant:
	if not _project_open():
		return McpToolResult.error("No project is open: editor_request new_project or open_project first.")
	var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": "build"}))))
	var failed := _outcome_error(answer, "editor_build")
	if failed != null:
		return failed
	var ended := await _operation_end(int(answer.get("outcome", {}).get("operation", 0)), ctx)
	var build: Dictionary = _query("operation").get("build", {})
	build["operation"] = ended
	build["ok"] = String(ended.get("end", "")) == "done" and bool(build.get("ok", false))
	if String(ended.get("end", "")) == "cancelled" or ended.is_empty():
		return McpToolResult.error("The build was cancelled before it landed.", build)
	if not bool(build["ok"]):
		return McpToolResult.error("The build failed; its diagnostics and editor_query problems say why.", build)
	return build


func _tool_editor_play(args: Dictionary, ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	match op:
		"start":
			if not _project_open():
				return McpToolResult.error("No project is open: editor_request new_project or open_project first.")
			if String(_run().get("state", "")) != "stopped":
				return McpToolResult.error("The game is already running; editor_play op=stop first.")
			# Play builds first: its build's operation is awaited as editor_build awaits it, and the
			# game starts on the poll the build lands.
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": "play"}))))
			var failed := _outcome_error(answer, "editor_play op=start")
			if failed != null:
				return failed
			await _operation_end(int(answer.get("outcome", {}).get("operation", 0)), ctx)
			var run := _run()
			if String(run.get("state", "")) != "running":
				return McpToolResult.error("Play did not start; the build or the launch refused (editor_query "
						+ "problems and output).", _query("operation").get("build", {}))
			return run
		"stop":
			app.call("request_json", JSON.stringify({"kind": "stop_play"}))
			var deadline := Time.get_ticks_msec() + STOP_WAIT_MS
			while String(_run().get("state", "")) != "stopped" and Time.get_ticks_msec() < deadline \
					and not ctx.cancelled:
				await ctx.frames(1)
			return _run()
		"state":
			return _run()
		_:
			return McpToolResult.error("Unknown editor_play op '%s'." % op)


func _tool_editor_menu_preview(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	match op:
		"state":
			return _menu_preview_state()
		"rects", "notes":
			var offset: Variant = _integer_number(args.get("offset", 0))
			var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
			if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
				return McpToolResult.error("editor_menu_preview op=%s takes offset >= 0 and limit from 1 to %d." % [op, EditorMcpCatalog.PAGE_MAX])
			var preview := _menu_preview()
			var key := "items" if op == "rects" else "notes"
			var items: Array = preview.get(key, [])
			var end := int(offset) + int(limit)
			return {
				"status": preview.get("status", ""),
				"reason": preview.get("reason", ""),
				"current": preview.get("current", false),
				"count": items.size(),
				"offset": int(offset),
				"next_offset": end if end < items.size() else null,
				key: items.slice(int(offset), end),
			}
		"hit":
			var x: Variant = _finite_number(args.get("x"))
			var y: Variant = _finite_number(args.get("y"))
			if x == null or y == null:
				return McpToolResult.error("editor_menu_preview op=hit requires numbers x and y (800x600 design units).")
			var hit: Variant = _parsed(String(app.call("menu_preview_hit_json", float(x), float(y))))
			if not (hit is Dictionary):
				return McpToolResult.error("The editor returned an invalid answer.")
			return hit
		"options":
			var options := {}
			for key in ["width", "height", "force_id"]:
				if args.has(key):
					var number: Variant = _integer_number(args[key])
					if number == null:
						return McpToolResult.error("editor_menu_preview option %s must be an integer." % key)
					options[key] = int(number)
			for key in ["show_hidden", "checked", "popup_open", "focus"]:
				if args.has(key):
					options[key] = bool(args[key])
			if args.has("force_state"):
				options["force_state"] = String(args["force_state"])
			if not bool(app.call("set_menu_preview_options", options)):
				return McpToolResult.error("editor_menu_preview op=options: an option is out of range or unknown "
						+ "(width and height 1..8192, force_state normal, mouseover, selected or disabled).")
			return _menu_preview_state()
		"drag", "nudge":
			var id: Variant = _integer_number(args.get("id"))
			var dx: Variant = _integer_number(args.get("dx"))
			var dy: Variant = _integer_number(args.get("dy"))
			if id == null or int(id) < 1 or dx == null or dy == null:
				return McpToolResult.error("editor_menu_preview op=%s requires id (a window of the previewed screen) " % op
						+ "and whole numbers dx and dy (design units).")
			var handle := "move"
			var snap := false
			if op == "drag":
				handle = String(args.get("handle", ""))
				if not EditorMcpCatalog.MENU_PREVIEW_HANDLES.has(handle):
					return McpToolResult.error("editor_menu_preview op=drag requires handle: %s." % ", ".join(PackedStringArray(EditorMcpCatalog.MENU_PREVIEW_HANDLES)))
				snap = bool(args.get("snap", true))
			if not bool(app.call("menu_preview_drag", int(id), handle, int(dx), int(dy), snap)):
				return McpToolResult.error("editor_menu_preview op=%s: the preview is not showing that window " % op
						+ "(select its screen and let the preview catch up), or the drag leaves it no area.")
			return _menu_preview_state()
		"arrange":
			var ids: Variant = args.get("ids")
			var arrange := String(args.get("arrange", ""))
			if not (ids is Array) or not EditorMcpCatalog.ARRANGE_OPS.has(arrange):
				return McpToolResult.error("editor_menu_preview op=arrange requires ids (windows of the previewed screen, "
						+ "the first the one the others align to) and arrange: %s."
						% ", ".join(PackedStringArray(EditorMcpCatalog.ARRANGE_OPS)))
			var windows := PackedInt64Array()
			for item: Variant in ids as Array:
				var window: Variant = _integer_number(item)
				if window == null or int(window) < 1:
					return McpToolResult.error("editor_menu_preview op=arrange: every id is a window's record id.")
				windows.append(int(window))
			if not bool(app.call("menu_preview_arrange", windows, arrange)):
				return McpToolResult.error("editor_menu_preview op=arrange: too few windows (two to align, three to "
						+ "distribute, one to reorder), or a window the preview is not showing (select its screen "
						+ "and let the preview catch up).")
			return _menu_preview_state()
		_:
			return McpToolResult.error("Unknown editor_menu_preview op '%s'." % op)


func _menu_preview() -> Dictionary:
	var preview: Variant = _parsed(String(app.call("get_menu_preview_json")))
	return preview if preview is Dictionary else {}


## The menu's viewport envelope without its items (the widgets) and notes, which op=rects and
## op=notes page: `count` the widgets', `note_count` the notes'.
func _menu_preview_state() -> Dictionary:
	var preview := _menu_preview()
	preview.erase("items")
	preview.erase("notes")
	preview.erase("offset")
	preview.erase("next_offset")
	return preview


func _tool_editor_model_preview(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	match op:
		"state":
			return _model_preview()
		"options":
			var options := {}
			for key in ["lod", "ctrl", "playing", "overlays", "time_ms", "rig_model", "clip_ticks"]:
				if args.has(key):
					options[key] = args[key]
			if not bool(app.call("set_model_preview_options", options)):
				return McpToolResult.error("editor_model_preview op=options takes lod (a level >= 0 or "
						+ "\"auto\"), ctrl (an object of register -> number), playing (a boolean), overlays "
						+ "({user_points, lights, pivots} booleans), time_ms (>= 0), rig_model (a model's file "
						+ "name, \"\" for the paired one) and clip_ticks (>= 0).")
			return _model_preview()
		"camera":
			var camera := {}
			for key in ["yaw", "pitch", "distance", "target", "frame", "width", "height"]:
				if args.has(key):
					camera[key] = args[key]
			if not bool(app.call("set_model_preview_camera", camera)):
				return McpToolResult.error("editor_model_preview op=camera takes numbers yaw, pitch, "
						+ "distance (> 0), target [x, y, z], frame (a boolean), width and height (1..8192).")
			return _model_preview()
		"hit":
			var x: Variant = args.get("x")
			var y: Variant = args.get("y")
			if not (x is float or x is int) or not (y is float or y is int):
				return McpToolResult.error("editor_model_preview op=hit requires numbers x and y (device pixels).")
			var hit: Variant = _parsed(String(app.call("model_preview_hit_json", float(x), float(y))))
			return hit if hit is Dictionary else {}
		"drag":
			var id: Variant = _integer_number(args.get("id"))
			var at_x: Variant = args.get("x")
			var at_y: Variant = args.get("y")
			var snap: Variant = args.get("snap", 0.0)
			var handle := String(args.get("handle", "place"))
			if id == null or not (at_x is float or at_x is int) or not (at_y is float or at_y is int) \
					or not (snap is float or snap is int) or not EditorMcpCatalog.MODEL_PREVIEW_HANDLES.has(handle):
				return McpToolResult.error("editor_model_preview op=drag requires id (a user point or light record), "
						+ "numbers x and y (device pixels), handle place or axis, snap >= 0.")
			if not bool(app.call("model_preview_drag", int(id), handle, float(at_x), float(at_y), float(snap))):
				return McpToolResult.error("editor_model_preview op=drag: the preview is not showing that record at "
						+ "the model's current revision, or it has no such handle (a pivot is geometry; an omni "
						+ "light has no axis).")
			return _model_preview()
		_:
			return McpToolResult.error("Unknown editor_model_preview op '%s'." % op)


func _model_preview() -> Dictionary:
	var preview: Variant = _parsed(String(app.call("get_model_preview_json")))
	return preview if preview is Dictionary else {}


func _tool_editor_screenshot(args: Dictionary, ctx: McpToolContext) -> Variant:
	var max_dim: Variant = _integer_number(args.get("max_dim", McpScreenshot.DEFAULT_MAX_DIM))
	var format: Variant = args.get("format", "webp")
	var quality: Variant = _finite_number(args.get("quality", McpScreenshot.DEFAULT_QUALITY))
	if max_dim == null or typeof(format) != TYPE_STRING or quality == null:
		return McpToolResult.error(
				"editor_screenshot requires an integer max_dim, a string format, and a numeric quality.")
	var viewport: Viewport = app.get_viewport() if app.is_inside_tree() else null
	var outcome: Dictionary = await McpScreenshot.capture(viewport, {
		"max_dim": int(max_dim),
		"format": String(format),
		"quality": float(quality),
	}, func() -> bool: return ctx.cancelled)
	if ctx.cancelled:
		return McpToolResult.error("Editor screenshot was cancelled after its request timed out.")
	if not bool(outcome.get("ok", false)):
		return McpToolResult.error(String(outcome.get("error", "Editor screenshot failed.")))
	var caption := "OpenNova Editor (%dx%d)" % [int(outcome["width"]), int(outcome["height"])]
	if outcome.has("warning"):
		caption += " " + String(outcome["warning"])
	return McpToolResult.image(outcome["bytes"], outcome["mime"], caption)


func _tool_editor_logs(args: Dictionary, ctx: McpToolContext) -> Variant:
	if service == null or service.log_hub == null:
		return McpToolResult.error("The editor's logs are unavailable.")
	var hub: McpLogHub = service.log_hub
	var session: Dictionary = service.server.session(String(ctx.args.get("_session_id", "")))
	var cursor := int(args.get("cursor", -1))
	if cursor < 0:
		cursor = int(session.get("log_cursor", 0)) if not session.is_empty() else 0
	var limit: Variant = _integer_number(args.get("limit", 200))
	if limit == null or int(limit) < 1 or int(limit) > 2000:
		return McpToolResult.error("editor_logs takes limit from 1 to 2000.")
	var page := hub.get_entries(cursor, int(limit), PackedStringArray(["server", "script"]))
	if not session.is_empty():
		session["log_cursor"] = int(page["next_cursor"])
	return page


## A query's answer as the tool's result, or the query's refusal as the tool's error.
func _answer(name: String, params: Dictionary) -> Variant:
	var answer: Variant = _parsed(String(app.call("query_json", name, JSON.stringify(params))))
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if (answer as Dictionary).has("error"):
		return McpToolResult.error(String(answer["error"]))
	return answer


## A query's answer ({} when it was refused).
func _query(name: String, params := {}) -> Dictionary:
	var answer: Variant = _parsed(String(app.call("query_json", name, JSON.stringify(params))))
	return answer if answer is Dictionary and not (answer as Dictionary).has("error") else {}


func _project_open() -> bool:
	return bool(_query("state", {"sections": ["project"]}).get("project", {}).get("open", false))


## The run section: Play's state, pid, port and exit.
func _run() -> Dictionary:
	return _query("state", {"sections": ["run"]}).get("run", {})


## The session's JSON as a Variant with whole numbers as integers: Godot's parser
## reads every number as a float, and an id of 3.0 is nobody's idea of an id.
static func _parsed(text: String) -> Variant:
	return _whole_numbers(JSON.parse_string(text))


static func _whole_numbers(value: Variant) -> Variant:
	match typeof(value):
		TYPE_FLOAT:
			return int(value) if float(value) == floorf(float(value)) and absf(float(value)) < 9007199254740992.0 else value
		TYPE_ARRAY:
			var items: Array = []
			for item in value:
				items.append(_whole_numbers(item))
			return items
		TYPE_DICTIONARY:
			var out := {}
			for key in value:
				out[key] = _whole_numbers(value[key])
			return out
		_:
			return value


## A request's answer as a tool error when the request did not happen: `ok` only says
## it read; the outcome (session_json's action_outcome_to_json) says whether it was
## refused or did not finish (its findings say why) or waits on the unsaved-changes
## prompt. null when it was done.
func _outcome_error(answer: Variant, what: String) -> McpToolResult:
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if not bool(answer.get("ok", false)):
		return McpToolResult.error(String(answer.get("error", "The request was refused.")))
	var outcome: Dictionary = answer.get("outcome", {})
	if bool(outcome.get("unsaved_prompt", false)):
		var prompt: Dictionary = _query("state", {"sections": ["dialogs"]}).get("dialogs", {}).get("unsaved_prompt", {})
		var choices := "save, discard or cancel" if bool(prompt.get("can_discard", true)) else "save or cancel"
		var files := ", ".join(PackedStringArray(prompt.get("files", [])))
		return McpToolResult.error("%s waits on unsaved changes (%s): editor_request resolve_unsaved with " % [what, files]
				+ "choice %s." % choices, prompt)
	if bool(outcome.get("done", false)):
		return null
	var findings: Array = outcome.get("findings", [])
	var reason := ""
	if not findings.is_empty():
		reason = " (%s: %s)" % [String(findings[0].get("code", "")), String(findings[0].get("message", ""))]
	return McpToolResult.error("%s did not go through%s." % [what, reason], findings)


## Frames until the operation `id` ends, the main thread never held: what it came to (the
## operation query's last_operation: id, kind, end, findings), or {} when the call is cancelled or
## another operation's outcome stands in its place.
func _operation_end(id: int, ctx: McpToolContext) -> Dictionary:
	while not ctx.cancelled:
		var state := _query("operation")
		if not state.is_empty():
			var running: Dictionary = state.get("operation", {})
			if id == 0 or not bool(running.get("running", false)) or int(running.get("id", 0)) != id:
				var last: Variant = state.get("last_operation")
				if last is Dictionary and int((last as Dictionary).get("id", 0)) == id:
					return last
				return {}
		await ctx.frames(1)
	return {}


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
