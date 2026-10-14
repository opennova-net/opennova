class_name EditorMcpTools
extends RefCounted

## The editor MCP's handlers over the editor's wire seam (EditorApp.request_json and query_json,
## ADR 0046 S13 A5): a request and a query cross as JSON text the portable session marshals, so
## this module forwards and waits; it never reaches into the session, and it pages nothing (the
## queries page their lists). editor_viewport (S13 V7) is the same two seams under one tool: its
## reads the viewport query, its writes set_viewport and edit_in_viewport.

## How long editor_request's `wait` awaits the operation and the validation, at most and by default
## (S13 A3): past it the answer says timed_out, with the operation and the validation as they stand. Below
## the tool's own budget, so a long wait always answers as timed_out (S26 C2: a wait_ms past the budget was
## cut by the registry with a bare error, the handler cancelled).
const REQUEST_WAIT_MS := EditorMcpCatalog.REQUEST_WAIT_MAX_MS

var service: EditorMcpService
var app: Node
## editor_viewport's reads: the viewport query's ops, as the catalog lists them (register_all).
var _viewport_reads: Array[String] = []


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
		"editor_viewport": _tool_editor_viewport,
		"editor_screenshot": _tool_editor_screenshot,
		"editor_logs": _tool_editor_logs,
	}


func register_all(registry: McpToolRegistry) -> void:
	var handlers := _handlers()
	_viewport_reads = EditorMcpCatalog.viewport_reads(EditorMcpCatalog.catalog_of(app))
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


## A request raised as the windows raise it. It answers at once, the operation it started or joined
## named in its outcome (S13 A1, A3: a project opened, a refresh, an import's plan and its write, a
## rename's commit, a build); `wait` (the tool's, not the request's) awaits that operation's end and
## the validation the polls step after it (or after an edit: no request runs it), frames passing the
## while, and answers with what the operation came to (operation: the last_operation block) and the
## status and view_revision as it left them. `wait_ms` bounds the wait (REQUEST_WAIT_MS, also its
## most): past it the answer says timed_out, its operation the running operation's state and its
## validation the validation's; the caller polls editor_query operation from there.
func _tool_editor_request(args: Dictionary, ctx: McpToolContext) -> Variant:
	var request := args.duplicate()
	request.erase("_session_id")
	var wait := bool(request.get("wait", false))
	var wait_ms := clampi(int(request.get("wait_ms", REQUEST_WAIT_MS)), 0, REQUEST_WAIT_MS)
	request.erase("wait")
	request.erase("wait_ms")
	# Unsorted: a replace_list record's fields are set in the order written, and JSON.stringify
	# sorts a Dictionary's keys unless told not to.
	var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request, "", false))))
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if not bool(answer.get("ok", false)):
		return McpToolResult.error(String(answer.get("error", "The request was refused.")))
	var id := int((answer as Dictionary).get("outcome", {}).get("operation", 0))
	if wait:
		var deadline := Time.get_ticks_msec() + maxi(wait_ms, 0)
		var ended := {}
		if id != 0:
			ended = await _operation_end(id, ctx, deadline)
		var settled := (id == 0 or not ended.is_empty()) and await _validation_end(ctx, deadline)
		if settled:
			if id != 0:
				answer["operation"] = ended
		else:
			var state := _query("operation")
			answer["timed_out"] = true
			answer["operation"] = state.get("operation", {})
			answer["validation"] = state.get("validation", {})
		var status := _query("state", {"sections": ["status"]})
		answer["status"] = String(status.get("status", {}).get("status", answer.get("status", "")))
		answer["view_revision"] = int(status.get("view_revision", answer.get("view_revision", 0)))
	return _whole(answer)


## editor_viewport (S13 V7): a document's viewport, `op` dispatched to the session's two seams. A read
## (the viewport query's ops, as the catalog lists them) is the viewport query, its params flat beside
## op as the query takes them; a write is a request (EditorMcpCatalog.VIEWPORT_WRITES: options and
## camera a set_viewport of the viewport's state, seek a set_viewport of the clock alone, drag,
## command and drop an edit_in_viewport; `kind` the member of the request that names the viewport's kind),
## answered as editor_request answers it (a request that did not read a tool error, one refused ok with
## its outcome not done) with the viewport's state after it as `viewport`.
func _tool_editor_viewport(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var params := args.duplicate()
	params.erase("_session_id")
	var op := String(params.get("op", ""))
	if _viewport_reads.has(op):
		return _answer("viewport", params)
	if not EditorMcpCatalog.VIEWPORT_WRITES.has(op):
		var ops: Array[String] = _viewport_reads.duplicate()
		for name: String in EditorMcpCatalog.VIEWPORT_WRITES:
			ops.append(name)
		return McpToolResult.error("editor_viewport has no op '%s' (%s)." % [op, ", ".join(PackedStringArray(ops))])
	var write: Dictionary = EditorMcpCatalog.VIEWPORT_WRITES[op]
	var pathless := bool(write.get("pathless", false))
	var takes: Array = write["takes"]
	var request := {"kind": String(write["kind"])}
	var members := {}
	var kind: Variant = null
	for key: String in params:
		if key == "op":
			continue
		if (key == "path" or key == "kind") and not pathless:
			if key == "path":
				request["path"] = params[key]
			else:
				kind = params[key]
		elif takes.has(key):
			members[key] = params[key]
		else:
			var taken: Array = takes.duplicate() if pathless else ["path", "kind"] + takes
			return McpToolResult.error("editor_viewport op=%s takes no \"%s\" (it takes %s)."
					% [op, key, ", ".join(PackedStringArray(taken))])
	if not members.has(write["needs"]):
		return McpToolResult.error("editor_viewport op=%s needs \"%s\"." % [op, String(write["needs"])])
	# A set_viewport carries the change as its viewport object, an edit_in_viewport its drag or
	# command; the kind goes in the one its write names.
	if String(write["kind"]) == "set_viewport":
		request["viewport"] = members
	else:
		request.merge(members)
	if kind != null:
		var named: Variant = request.get(String(write["kind_in"]))
		if not (named is Dictionary):
			return McpToolResult.error("editor_viewport op=%s: \"%s\" must be an object." % [op, String(write["kind_in"])])
		(named as Dictionary)["kind"] = kind
	# Unsorted: a change's members as written.
	var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request, "", false))))
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if not bool(answer.get("ok", false)):
		return McpToolResult.error(String(answer.get("error", "The request was refused.")))
	# The viewport the write named, after it (a seek's: the active document's, none with nothing to show).
	var state := {"op": "state"}
	if request.has("path"):
		state["path"] = request["path"]
	if kind != null:
		state["kind"] = kind
	var after := _query("viewport", state)
	if not after.is_empty():
		answer["viewport"] = after
	return _whole(answer)

func _tool_editor_screenshot(args: Dictionary, ctx: McpToolContext) -> Variant:
	var viewport: Viewport = app.get_viewport() if app.is_inside_tree() else null
	return await McpScreenshot.tool_capture(viewport, args, ctx,
			"editor_screenshot", "Editor screenshot", "OpenNova Editor")


func _tool_editor_logs(args: Dictionary, ctx: McpToolContext) -> Variant:
	if service == null or service.log_hub == null:
		return McpToolResult.error("The editor's logs are unavailable.")
	var limit: Variant = McpToolArgs.integer_number(args.get("limit", 200))
	if limit == null or int(limit) < 1 or int(limit) > 2000:
		return McpToolResult.error("editor_logs takes limit from 1 to 2000.")
	return service.log_hub.session_page(
			service.server.session(String(ctx.args.get("_session_id", ""))),
			int(args.get("cursor", -1)), int(limit), PackedStringArray(["server", "script"]))


## A query's answer as the tool's result, or the query's refusal as the tool's error.
func _answer(name: String, params: Dictionary) -> Variant:
	var text := String(app.call("query_json", name, JSON.stringify(params)))
	var answer: Variant = _parsed(text)
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if (answer as Dictionary).has("error"):
		return McpToolResult.error(String(answer["error"]))
	return _whole(answer, text)


## The session's answer as the tool's result, whole (S26 C1): the session pages its lists and its JSON is
## JSON already, so the game MCP's McpJson.sanitize (a list cut at 200 entries with a string in it, a depth
## past 24 turned to "<max depth>", a string clipped at 8192 characters) only cut it: a menu's tree, a
## model's parts, a font's 224 glyphs and a texture thumbnail's PNG came back broken. `text` is the
## session's own JSON text when the caller has it (the answer as it was not changed since).
static func _whole(answer: Dictionary, text := "") -> McpToolResult:
	var result := McpToolResult.new()
	result.add_text(text if not text.is_empty() else JSON.stringify(answer, "", false))
	result.structured = answer
	return result


## A query's answer ({} when it was refused).
func _query(name: String, params := {}) -> Dictionary:
	var answer: Variant = _parsed(String(app.call("query_json", name, JSON.stringify(params))))
	return answer if answer is Dictionary and not (answer as Dictionary).has("error") else {}


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


## Frames until the validation the polls step has ended (S13 A3: the first one after a project
## opens, above all), or `deadline` (ticks) passed: true when it ended, the Problems rows the
## project's then.
func _validation_end(ctx: McpToolContext, deadline: int) -> bool:
	while not ctx.cancelled and Time.get_ticks_msec() < deadline:
		if not bool(_query("operation").get("validation", {}).get("running", false)):
			return true
		await ctx.frames(1)
	return not bool(_query("operation").get("validation", {}).get("running", false))


## Frames until the operation `id` ends, the main thread never held: what it came to (the
## operation query's last_operation: id, kind, end, findings), or {} when the call is cancelled,
## `deadline` (ticks, 0 for none) passed, or another operation's outcome stands in its place.
func _operation_end(id: int, ctx: McpToolContext, deadline := 0) -> Dictionary:
	while not ctx.cancelled and (deadline == 0 or Time.get_ticks_msec() < deadline):
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
