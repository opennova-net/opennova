class_name EditorMcpTools
extends RefCounted

## The editor MCP's handlers over the editor's typed seam (EditorApp). Every record
## and view crosses as JSON text the portable session marshals (session_json), so
## this module reads keys and forwards; it never reaches into the session.

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
		"editor_document": _tool_editor_document,
		"editor_problems": _tool_editor_problems,
		"editor_build": _tool_editor_build,
		"editor_play": _tool_editor_play,
		"editor_graph": _tool_editor_graph,
		"editor_menu_preview": _tool_editor_menu_preview,
		"editor_model_preview": _tool_editor_model_preview,
		"editor_menu": _tool_editor_menu,
		"editor_screenshot": _tool_editor_screenshot,
		"editor_logs": _tool_editor_logs,
	}


func register_all(registry: McpToolRegistry) -> void:
	var handlers := _handlers()
	for def in EditorMcpCatalog.definitions():
		if not handlers.has(def.name):
			push_error("EditorMcpTools has no handler for cataloged tool '%s'." % def.name)
			continue
		registry.register(def, handlers[def.name])


func _tool_editor_state(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var cursor: Variant = _integer_number(args.get("output_cursor", 0))
	var limit: Variant = _integer_number(args.get("output_limit", EditorMcpCatalog.PAGE_MAX))
	if cursor == null or int(cursor) < 0 or limit == null or int(limit) < 0 or int(limit) > EditorMcpCatalog.PAGE_MAX:
		return McpToolResult.error("editor_state takes output_cursor >= 0 and output_limit from 0 to %d." % EditorMcpCatalog.PAGE_MAX)
	var import_offset: Variant = _integer_number(args.get("import_offset", 0))
	var import_limit: Variant = _integer_number(args.get("import_limit", EditorMcpCatalog.PAGE_MAX))
	if import_offset == null or int(import_offset) < 0 or import_limit == null or int(import_limit) < 0 \
			or int(import_limit) > EditorMcpCatalog.PAGE_MAX:
		return McpToolResult.error("editor_state takes import_offset >= 0 and import_limit from 0 to %d." % EditorMcpCatalog.PAGE_MAX)
	var files_offset: Variant = _integer_number(args.get("files_offset", 0))
	var files_limit: Variant = _integer_number(args.get("files_limit", EditorMcpCatalog.PAGE_MAX))
	if files_offset == null or int(files_offset) < 0 or files_limit == null or int(files_limit) < 0 \
			or int(files_limit) > EditorMcpCatalog.PAGE_MAX:
		return McpToolResult.error("editor_state takes files_offset >= 0 and files_limit from 0 to %d." % EditorMcpCatalog.PAGE_MAX)
	var view := _view(int(cursor), int(limit), int(import_offset), int(import_limit))
	var project: Variant = view.get("project")
	if project is Dictionary and (project as Dictionary).has("files"):
		var page := _files_page(project as Dictionary, int(files_offset), int(files_limit))
		project["files"] = page["files"]
		project["file_count"] = page["file_count"]
		project["file_offset"] = page["file_offset"]
	return view


func _tool_editor_request(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var request := args.duplicate()
	request.erase("_session_id")
	var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request))))
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if not bool(answer.get("ok", false)):
		return McpToolResult.error(String(answer.get("error", "The request was refused.")))
	return answer


func _tool_editor_document(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	var path := String(args.get("path", ""))
	match op:
		"list":
			var offset: Variant = _integer_number(args.get("offset", 0))
			var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
			if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
				return McpToolResult.error("editor_document op=list takes offset >= 0 and limit from 1 to %d." % EditorMcpCatalog.PAGE_MAX)
			var view := _view(0, 0)
			var listed := _files_page(view.get("project", {}), int(offset), int(limit))
			listed["open"] = view.get("documents", [])
			listed["active_document"] = view.get("active_document", "")
			return listed
		"open", "create", "close", "reload":
			if path.is_empty() and op != "close" and op != "reload":
				return McpToolResult.error("editor_document op=%s requires path." % op)
			var request := {"kind": "create_file" if op == "create" else op + "_document"}
			if not path.is_empty():
				request["path"] = path
			if op == "create" and args.has("kind"):
				request["file_kind"] = String(args["kind"])
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request))))
			var failed := _outcome_error(answer, "editor_document op=%s" % op)
			if failed != null:
				return failed
			if op == "open" or op == "create":
				var document: Variant = _parsed(String(app.call("get_document_json", path, false)))
				if document == null and op == "create":
					return answer # made, of a kind the editor opens no document for (a font)
				if document == null:
					return McpToolResult.error("%s did not open; see editor_problems." % path, answer)
				return document
			return answer
		"rows":
			var offset: Variant = _integer_number(args.get("offset", 0))
			var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
			if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
				return McpToolResult.error("editor_document op=rows takes offset >= 0 and limit from 1 to %d." % EditorMcpCatalog.PAGE_MAX)
			var document: Variant = _parsed(String(app.call("get_document_json", path, true)))
			if document == null:
				return McpToolResult.error(
						"No open document%s: editor_document op=open first." % ("" if path.is_empty() else " " + path))
			# One page of rows: the sanitizer caps a list, so a retail catalog pages.
			var rows: Array = document.get("rows", [])
			document["row_offset"] = int(offset)
			document["rows"] = rows.slice(int(offset), int(offset) + int(limit))
			return document
		"record":
			var id: Variant = _integer_number(args.get("id"))
			if id == null or int(id) <= 0:
				return McpToolResult.error("editor_document op=record requires a record id.")
			var record: Variant = _parsed(String(app.call("get_record_json", int(id))))
			if record == null:
				return McpToolResult.error("No record %d in the active document." % int(id))
			return record
		"search":
			var text := String(args.get("text", ""))
			if text.is_empty():
				return McpToolResult.error("editor_document op=search requires text.")
			var bounds: Variant = _page_bounds(args, "editor_document op=search")
			if bounds is McpToolResult:
				return bounds
			var found: Variant = _parsed(String(app.call("search_document_json", path, text, bool(args.get("match_case", false)))))
			if not (found is Dictionary):
				return McpToolResult.error(
						"No open document%s: editor_document op=open first." % ("" if path.is_empty() else " " + path))
			return _page(found.get("hits", []), "hits", bounds[0], bounds[1])
		"choices", "targets":
			# The picker's names for a reference field, or where its Go to leads with the value
			# it holds: the answer whole, its list a page.
			var id: Variant = _integer_number(args.get("id"))
			var field := String(args.get("field", ""))
			if id == null or int(id) <= 0 or field.is_empty():
				return McpToolResult.error("editor_document op=%s requires id and field." % op)
			var bounds: Variant = _page_bounds(args, "editor_document op=%s" % op)
			if bounds is McpToolResult:
				return bounds
			var method := "get_reference_choices_json" if op == "choices" else "get_reference_targets_json"
			var answer: Variant = _parsed(String(app.call(method, int(id), field)))
			if not (answer is Dictionary):
				return McpToolResult.error("No record %d with a field '%s' in the active document." % [int(id), field])
			var page := _page(answer.get(op, []), op, bounds[0], bounds[1])
			answer.merge(page, true)
			return answer
		"find":
			var symbol := String(args.get("symbol", ""))
			if symbol.is_empty():
				return McpToolResult.error("editor_document op=find requires symbol.")
			var scope := String(args.get("scope", ""))
			var id: int = app.call("find_record", symbol, scope)
			if id <= 0:
				return McpToolResult.error("No record named '%s'%s in the active document."
						% [symbol, (" in " + scope) if not scope.is_empty() else ""])
			return {"id": id, "name": String(app.call("get_record_name", id))}
		"clear", "write":
			var id: Variant = _integer_number(args.get("id"))
			var field := String(args.get("field", ""))
			if id == null or int(id) <= 0 or field.is_empty():
				return McpToolResult.error("editor_document op=%s requires id and field." % op)
			var method := "clear_field" if op == "clear" else "write_field"
			if not bool(app.call(method, int(id), field)):
				return McpToolResult.error(
						"The field was not %s (unknown, always written, or a blocked document); see editor_problems."
						% ("cleared" if op == "clear" else "written"),
						_last_problems())
			return {"ok": true, "id": int(id), "field": field, "value": app.call("get_field", int(id), field)}
		"revert":
			var id: Variant = _integer_number(args.get("id"))
			var field := String(args.get("field", ""))
			if id == null or int(id) <= 0 or field.is_empty():
				return McpToolResult.error("editor_document op=revert requires id and field.")
			var record: Variant = _parsed(String(app.call("get_record_json", int(id))))
			if not (record is Dictionary):
				return McpToolResult.error("No record %d in the active document." % int(id))
			var request := {"kind": "revert_to_saved", "edits": [{"row": int(record["row"]), "kind": int(record["kind"]),
					"child": int(record["child"]), "field": field}]}
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request))))
			var failed := _outcome_error(answer, "editor_document op=revert")
			if failed != null:
				return failed
			return {"ok": true, "id": int(id), "field": field, "value": app.call("get_field", int(id), field)}
		"set":
			var id: Variant = _integer_number(args.get("id"))
			var field := String(args.get("field", ""))
			if id == null or int(id) <= 0 or field.is_empty() or not args.has("value"):
				return McpToolResult.error("editor_document op=set requires id, field and value.")
			var value: Variant = args["value"]
			if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT \
					and typeof(value) != TYPE_STRING and typeof(value) != TYPE_BOOL:
				return McpToolResult.error("value must be a number, a string or a bool.")
			if typeof(value) == TYPE_FLOAT and float(value) == floorf(float(value)):
				value = int(value)
			if not bool(app.call("set_field", int(id), field, value)):
				return McpToolResult.error(
						"The field was not set (unknown field, a bad value, or a blocked document); see editor_problems.",
						_last_problems())
			return {"ok": true, "id": int(id), "field": field, "value": app.call("get_field", int(id), field)}
		"add":
			var kind := String(args.get("kind", ""))
			var parent: Variant = _integer_number(args.get("parent", 0))
			var position: Variant = _integer_number(args.get("position", -1))
			if kind.is_empty() or parent == null or int(parent) < 0 or position == null or int(position) < -1:
				return McpToolResult.error(
						"editor_document op=add requires kind (a record kind name), an integer parent >= 0 and position >= 0.")
			var id: int = app.call("add_record", kind, int(parent), int(position))
			if id <= 0:
				return McpToolResult.error(
						"No record was added (unknown kind '%s', or the parent holds no such records); see editor_problems." % kind,
						_last_problems())
			return {"ok": true, "id": id}
		"select":
			var id: Variant = _integer_number(args.get("id"))
			var mode := String(args.get("mode", "replace"))
			if id == null or int(id) <= 0:
				return McpToolResult.error("editor_document op=select requires a record id.")
			if not bool(app.call("select_record", int(id), mode)):
				return McpToolResult.error("No record %d in the active document, or an unknown mode '%s'." % [int(id), mode])
			var view := _view(0, 0)
			return {"ok": true, "selection": view.get("selection", {}), "selected": view.get("selected", [])}
		"copy", "cut":
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": op}))))
			var failed := _outcome_error(answer, "editor_document op=%s" % op)
			if failed != null:
				return failed
			return {"ok": true, "clipboard_bytes": int(_view(0, 0).get("clipboard_bytes", 0))}
		"paste":
			var parent: Variant = _integer_number(args.get("parent", 0))
			var position: Variant = _integer_number(args.get("position", -1))
			if parent == null or int(parent) < 0 or position == null or int(position) < -1:
				return McpToolResult.error("editor_document op=paste takes an integer parent >= 0 and position >= 0.")
			if not bool(app.call("paste_records", int(parent), int(position))):
				return McpToolResult.error("Nothing was pasted; see editor_problems.", _last_problems())
			return {"ok": true, "selected": _view(0, 0).get("selected", [])}
		"end_edit":
			app.call("end_edit")
			return {"ok": true}
		"duplicate", "remove", "move":
			if op == "duplicate" and not args.has("id"):
				# Every selected record, each right after itself, one step.
				var duplicated: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": "duplicate"}))))
				var refused := _outcome_error(duplicated, "editor_document op=duplicate")
				if refused != null:
					return refused
				return {"ok": true, "selected": _view(0, 0).get("selected", [])}
			var id: Variant = _integer_number(args.get("id"))
			if id == null or int(id) <= 0:
				return McpToolResult.error("editor_document op=%s requires a record id." % op)
			var done := false
			match op:
				"duplicate":
					done = bool(app.call("duplicate_record", int(id)))
				"remove":
					done = bool(app.call("remove_record", int(id)))
				"move":
					var position: Variant = _integer_number(args.get("position", 0))
					var parent: Variant = _integer_number(args.get("parent", 0))
					if position == null or int(position) < 0 or parent == null or int(parent) < 0:
						return McpToolResult.error("editor_document op=move requires position >= 0 (and parent >= 0).")
					done = bool(app.call("move_record", int(id), int(position), int(parent)))
			if not done:
				return McpToolResult.error("The %s was refused; see editor_problems." % op, _last_problems())
			return {"ok": true, "id": int(id)}
		"save":
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": "save", "path": path}))))
			var failed := _outcome_error(answer, "editor_document op=save")
			if failed != null:
				return failed
			return answer
		"save_all":
			var saved := bool(app.call("save_documents"))
			if not saved:
				return McpToolResult.error("Not every document saved; see editor_problems.", _last_problems())
			return {"ok": true}
		"undo":
			app.call("undo")
			return {"ok": true, "dirty": app.call("is_document_dirty")}
		"redo":
			app.call("redo")
			return {"ok": true, "dirty": app.call("is_document_dirty")}
		_:
			return McpToolResult.error("Unknown editor_document op '%s'." % op)


## The query goes to the editor whole (session_json's problem_query_from_json answers it as
## the Problems window does); only the page's size is the transport's, which caps a list.
func _tool_editor_problems(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var query := args.duplicate()
	query.erase("_session_id")
	var offset: Variant = _integer_number(query.get("offset", 0))
	var limit: Variant = _integer_number(query.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
	if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
		return McpToolResult.error("editor_problems takes offset >= 0 and limit from 1 to %d." % EditorMcpCatalog.PAGE_MAX)
	query["offset"] = int(offset)
	query["limit"] = int(limit)
	var answer: Variant = _parsed(String(app.call("get_problems_json", JSON.stringify(query))))
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if answer.has("error"):
		return McpToolResult.error("editor_problems: %s" % String(answer["error"]))
	return answer


## The build raised as the windows raise it, then frames awaited until its operation ends: the
## editor keeps drawing and answering editor_state (its operation block) the while. A build
## running already is joined, not started again.
func _tool_editor_build(_args: Dictionary, ctx: McpToolContext) -> Variant:
	if not bool(app.call("is_project_open")):
		return McpToolResult.error("No project is open: editor_request new_project or open_project first.")
	var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": "build"}))))
	var failed := _outcome_error(answer, "editor_build")
	if failed != null:
		return failed
	var ended := await _operation_end(int(answer.get("outcome", {}).get("operation", 0)), ctx)
	var build: Dictionary = _view(0, 0).get("build", {})
	build["operation"] = ended
	build["ok"] = String(ended.get("end", "")) == "done" and bool(build.get("ok", false))
	if String(ended.get("end", "")) == "cancelled" or ended.is_empty():
		return McpToolResult.error("The build was cancelled before it landed.", build)
	if not bool(build["ok"]):
		return McpToolResult.error("The build failed; its diagnostics and editor_problems say why.", build)
	return build


func _tool_editor_play(args: Dictionary, ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	match op:
		"start":
			if not bool(app.call("is_project_open")):
				return McpToolResult.error("No project is open: editor_request new_project or open_project first.")
			if String(app.call("get_play_state")) != "stopped":
				return McpToolResult.error("The game is already running; editor_play op=stop first.")
			# Play builds first: its build's operation is awaited as editor_build awaits it, and the
			# game starts on the poll the build lands.
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify({"kind": "play"}))))
			var failed := _outcome_error(answer, "editor_play op=start")
			if failed != null:
				return failed
			await _operation_end(int(answer.get("outcome", {}).get("operation", 0)), ctx)
			if String(app.call("get_play_state")) != "running":
				return McpToolResult.error("Play did not start; the build or the launch refused (editor_problems, "
						+ "editor_state's output).", _view(0, 0).get("build", {}))
			return _play_block()
		"stop":
			app.call("stop_play")
			var deadline := Time.get_ticks_msec() + STOP_WAIT_MS
			while String(app.call("get_play_state")) != "stopped" and Time.get_ticks_msec() < deadline \
					and not ctx.cancelled:
				await ctx.frames(1)
			return _play_block()
		"state":
			return _play_block()
		_:
			return McpToolResult.error("Unknown editor_play op '%s'." % op)


func _tool_editor_graph(args: Dictionary, _ctx: McpToolContext) -> Variant:
	if not bool(app.call("is_project_open")):
		return McpToolResult.error("No project is open: editor_request new_project or open_project first.")
	var op := String(args.get("op", ""))
	var path := String(args.get("path", ""))
	# The lists a page at a time: the transport caps a list at PAGE_MAX entries.
	var bounds: Variant = [0, 0]
	if op in EditorMcpCatalog.PAGED_GRAPH_OPS:
		bounds = _page_bounds(args, "editor_graph op=%s" % op)
		if bounds is McpToolResult:
			return bounds
	match op:
		"references":
			if path.is_empty():
				return McpToolResult.error("editor_graph op=references requires path.")
			var edges: Variant = _parsed(String(app.call("get_references_json", path)))
			var page := _page(edges if edges is Array else [], "edges", bounds[0], bounds[1])
			page["path"] = path
			return page
		"referrers", "usages":
			var kind := String(args.get("kind", ""))
			var name := String(args.get("name", ""))
			var edges: Variant
			if not path.is_empty():
				edges = _parsed(String(app.call("get_referrers_json" if op == "referrers" else "get_usages_json", path)))
			elif not kind.is_empty() and not name.is_empty():
				edges = _parsed(String(app.call("get_symbol_referrers_json", kind, name, String(args.get("scope", "")))))
			else:
				return McpToolResult.error("editor_graph op=%s requires path, or kind and name." % op)
			return _page(edges if edges is Array else [], "edges", bounds[0], bounds[1])
		"missing":
			var edges: Variant = _parsed(String(app.call("get_missing_references_json")))
			return _page(edges if edges is Array else [], "edges", bounds[0], bounds[1])
		"symbols":
			var symbols: Variant = _parsed(String(app.call("get_symbols_json", String(args.get("kind", "")))))
			return _page(symbols if symbols is Array else [], "symbols", bounds[0], bounds[1])
		"search":
			var text := String(args.get("text", ""))
			if text.is_empty():
				return McpToolResult.error("editor_graph op=search requires text.")
			var found: Variant = _parsed(String(app.call("search_project_json", text)))
			if not (found is Dictionary):
				return McpToolResult.error("The editor returned an invalid answer.")
			return _page(found.get("hits", []), "hits", bounds[0], bounds[1])
		"rename_symbol":
			var field := String(args.get("field", ""))
			var name := String(args.get("name", ""))
			if path.is_empty() or field.is_empty() or name.is_empty():
				return McpToolResult.error(
						"editor_graph op=rename_symbol requires path, locator, field (a symbol as op=symbols lists it) and name.")
			var dry_run := bool(args.get("dry_run", false))
			var request := {"kind": "preview_rename" if dry_run else "rename_symbol", "path": path,
					"locator": String(args.get("locator", "")), "field": field, "new_name": name}
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request))))
			var failed := _outcome_error(answer, "editor_graph op=rename_symbol")
			if failed != null:
				return failed
			if dry_run:
				return _view(0, 0).get("rename_preview", {})
			return answer
		"rename", "assign":
			var request := {}
			if op == "rename":
				var name := String(args.get("name", ""))
				if path.is_empty() or name.is_empty():
					return McpToolResult.error("editor_graph op=rename requires path and name.")
				request = {"kind": "rename_asset", "path": path, "new_name": name}
			else:
				var role := String(args.get("role", ""))
				if path.is_empty() or role.is_empty():
					return McpToolResult.error("editor_graph op=assign requires role and path.")
				request = {"kind": "assign_requirement", "path": path, "role": role}
			var answer: Variant = _parsed(String(app.call("request_json", JSON.stringify(request))))
			var failed := _outcome_error(answer, "editor_graph op=%s" % op)
			if failed != null:
				return failed
			return answer
		_:
			return McpToolResult.error("Unknown editor_graph op '%s'." % op)


func _tool_editor_menu_preview(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var op := String(args.get("op", ""))
	match op:
		"state":
			return _menu_preview_state()
		"rects":
			var offset: Variant = _integer_number(args.get("offset", 0))
			var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
			if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
				return McpToolResult.error("editor_menu_preview op=rects takes offset >= 0 and limit from 1 to %d." % EditorMcpCatalog.PAGE_MAX)
			var preview := _menu_preview()
			var widgets: Array = preview.get("widgets", [])
			return {
				"status": preview.get("status", ""),
				"current": preview.get("current", false),
				"count": widgets.size(),
				"offset": int(offset),
				"widgets": widgets.slice(int(offset), int(offset) + int(limit)),
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
		"notes", "render":
			var offset: Variant = _integer_number(args.get("offset", 0))
			var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
			if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
				return McpToolResult.error("editor_menu_preview op=%s takes offset >= 0 and limit from 1 to %d." % [op, EditorMcpCatalog.PAGE_MAX])
			var source: Dictionary = {}
			if op == "notes" and not args.has("path"):
				source = _menu_preview()
			else:
				var screen: Variant = _integer_number(args.get("screen"))
				if String(args.get("path", "")).is_empty() or screen == null or int(screen) < 1:
					return McpToolResult.error("editor_menu_preview op=%s requires path (a menu) and screen (its screen row id)." % op)
				var render: Variant = _parsed(String(app.call("get_menu_render_json", String(args["path"]), int(screen))))
				if not (render is Dictionary):
					return McpToolResult.error("The editor returned an invalid answer.")
				source = render as Dictionary
			var from := int(offset)
			var to := int(offset) + int(limit)
			var notes: Array = source.get("notes", [])
			if op == "notes":
				return {
					"status": source.get("status", ""),
					"current": source.get("current", false),
					"count": notes.size(),
					"offset": from,
					"notes": notes.slice(from, to),
				}
			var widgets: Array = source.get("widgets", [])
			source.erase("notes")
			source["note_count"] = notes.size()
			source["widget_count"] = widgets.size()
			source["offset"] = from
			source["widgets"] = widgets.slice(from, to)
			return source
		_:
			return McpToolResult.error("Unknown editor_menu_preview op '%s'." % op)


func _tool_editor_menu(args: Dictionary, _ctx: McpToolContext) -> Variant:
	if not bool(app.call("is_project_open")):
		return McpToolResult.error("No project is open: editor_request new_project or open_project first.")
	var op := String(args.get("op", ""))
	var path := String(args.get("path", ""))
	var offset: Variant = _integer_number(args.get("offset", 0))
	var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_MAX))
	if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
		return McpToolResult.error("editor_menu takes offset >= 0 and limit from 1 to %d." % EditorMcpCatalog.PAGE_MAX)
	match op:
		"tree":
			var tree: Variant = _parsed(String(app.call("get_menu_tree_json", path)))
			if not (tree is Dictionary):
				return McpToolResult.error(_no_menu(path))
			var menu: Dictionary = tree
			var only: Variant = _integer_number(args.get("screen", 0))
			if only == null or int(only) < 0:
				return McpToolResult.error("editor_menu op=tree takes screen, a screen's row id.")
			var screens: Array = []
			for screen: Variant in menu.get("screens", []):
				if not (screen is Dictionary):
					continue
				var entry: Dictionary = screen
				if int(only) > 0 and int(entry.get("id", 0)) != int(only):
					continue
				# One page of each screen's windows: the sanitizer caps a list.
				var windows: Array = entry.get("windows", [])
				entry["window_offset"] = int(offset)
				entry["windows"] = windows.slice(int(offset), int(offset) + int(limit))
				screens.append(entry)
			if int(only) > 0 and screens.is_empty():
				return McpToolResult.error("%s has no screen %d." % [String(menu.get("path", path)), int(only)])
			menu["screens"] = screens
			return menu
		"edit", "list":
			var request := {}
			if op == "edit":
				if not (args.get("edits") is Array):
					return McpToolResult.error("editor_menu op=edit requires edits, a list of "
							+ "{op, id, field, value, kind, parent, position, as}.")
				request["edits"] = args["edits"]
			else:
				var id: Variant = _integer_number(args.get("id"))
				if id == null or int(id) < 1 or String(args.get("list", "")).is_empty() or not (args.get("records") is Array):
					return McpToolResult.error("editor_menu op=list requires id (the record holding the list), list (its "
							+ "kind token: action, sound, items.item, ...) and records, a list of {field: value}.")
				request = {"id": int(id), "list": String(args["list"]), "records": args["records"]}
			# Unsorted: a list record's fields are set in the order written, and
			# JSON.stringify sorts a Dictionary's keys unless told not to.
			var answer: Variant = _parsed(String(app.call("edit_menu_json", path, JSON.stringify(request, "", false))))
			if not (answer is Dictionary):
				return McpToolResult.error("The editor returned an invalid answer.")
			var result: Dictionary = answer
			if not bool(result.get("ok", false)):
				return McpToolResult.error("editor_menu op=%s: %s" % [op, String(result.get("error", "refused"))])
			var failed := _outcome_error(result, "editor_menu op=%s" % op)
			if failed != null:
				return failed
			return result
		"analyze":
			var report: Variant = _parsed(String(app.call("get_menu_findings_json", path)))
			if not (report is Dictionary):
				return McpToolResult.error(_no_menu(path))
			var findings: Dictionary = report
			var severity := String(args.get("severity", ""))
			var rows: Array = []
			for problem: Variant in findings.get("problems", []):
				if problem is Dictionary and (severity.is_empty() or String((problem as Dictionary).get("severity", "")) == severity):
					rows.append(problem)
			findings["matching"] = rows.size()
			findings["offset"] = int(offset)
			findings["problems"] = rows.slice(int(offset), int(offset) + int(limit))
			return findings
		_:
			return McpToolResult.error("Unknown editor_menu op '%s' (%s)."
					% [op, ", ".join(PackedStringArray(EditorMcpCatalog.MENU_OPS))])


static func _no_menu(path: String) -> String:
	if path.is_empty():
		return "No menu is previewed or open: name one with path (a project-relative path or a logical name)."
	return "No menu '%s' in the project (a project-relative path or a logical name)." % path


func _menu_preview() -> Dictionary:
	var preview: Variant = _parsed(String(app.call("get_menu_preview_json")))
	return preview if preview is Dictionary else {}


## The preview without its widgets and notes (op=rects and op=notes page them).
func _menu_preview_state() -> Dictionary:
	var preview := _menu_preview()
	var widgets: Array = preview.get("widgets", [])
	var notes: Array = preview.get("notes", [])
	preview.erase("widgets")
	preview.erase("notes")
	preview["widget_count"] = widgets.size()
	preview["note_count"] = notes.size()
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


## A page of the project's scanned files: the result sanitizer caps a list at PAGE_MAX
## entries, so a project with more files is read a page at a time (file_count says how many).
static func _files_page(project: Dictionary, offset: int, limit: int) -> Dictionary:
	var files: Array = project.get("files", [])
	return {"file_count": files.size(), "file_offset": offset, "files": files.slice(offset, offset + limit)}


func _view(cursor: int, limit: int, import_offset := 0, import_limit := EditorMcpCatalog.PAGE_MAX) -> Dictionary:
	var view: Variant = _parsed(String(app.call("get_view_json", cursor, limit, import_offset, import_limit)))
	return view if view is Dictionary else {}


## One page of a list the editor answers whole (the transport caps a list): the total as
## count, the page's first as offset, the page's entries alone under `key`.
static func _page(list: Array, key: String, offset: int, limit: int) -> Dictionary:
	return {"count": list.size(), "offset": offset, key: list.slice(offset, offset + limit)}


## A paged op's offset and limit ([offset, limit]), or the tool's error when either is out of
## range: offset from 0, limit from 1 to PAGE_MAX (PAGE_DEFAULT when left out).
static func _page_bounds(args: Dictionary, what: String) -> Variant:
	var offset: Variant = _integer_number(args.get("offset", 0))
	var limit: Variant = _integer_number(args.get("limit", EditorMcpCatalog.PAGE_DEFAULT))
	if offset == null or int(offset) < 0 or limit == null or int(limit) < 1 or int(limit) > EditorMcpCatalog.PAGE_MAX:
		return McpToolResult.error("%s takes offset >= 0 and limit from 1 to %d." % [what, EditorMcpCatalog.PAGE_MAX])
	return [int(offset), int(limit)]


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
## it parsed; the outcome (session_json's action_outcome_to_json) says whether it was
## refused or did not finish (its findings say why) or waits on the unsaved-changes
## prompt. null when it was done.
func _outcome_error(answer: Variant, what: String) -> McpToolResult:
	if not (answer is Dictionary):
		return McpToolResult.error("The editor returned an invalid answer.")
	if not bool(answer.get("ok", false)):
		return McpToolResult.error(String(answer.get("error", "The request was refused.")))
	var outcome: Dictionary = answer.get("outcome", {})
	if bool(outcome.get("unsaved_prompt", false)):
		var waiting := _unsaved_prompt_error(what)
		if waiting != null:
			return waiting
	if bool(outcome.get("done", false)):
		return null
	var findings: Array = outcome.get("findings", [])
	var reason := ""
	if not findings.is_empty():
		reason = " (%s: %s)" % [String(findings[0].get("code", "")), String(findings[0].get("message", ""))]
	return McpToolResult.error("%s did not go through%s." % [what, reason], findings)


## The unsaved-changes prompt as a tool error while it is open (for `action`, a request kind
## token, when one is named): what waits, the files it lists and the answers it takes. null
## when no such prompt is open.
func _unsaved_prompt_error(what: String, action: String = "") -> McpToolResult:
	var prompt: Dictionary = _view(0, 0).get("unsaved_prompt", {})
	if not bool(prompt.get("open", false)) or (not action.is_empty() and String(prompt.get("action", "")) != action):
		return null
	var choices := "save, discard or cancel" if bool(prompt.get("can_discard", true)) else "save or cancel"
	var listed: Array = prompt.get("files", [])
	var files := ", ".join(PackedStringArray(listed))
	return McpToolResult.error("%s waits on unsaved changes (%s): editor_request resolve_unsaved with " % [what, files]
			+ "choice %s." % choices, prompt)


func _play_block() -> Dictionary:
	var play: Dictionary = _view(0, 0).get("play", {})
	play["ok"] = true
	return play


## Frames until the operation `id` ends, the main thread never held: what it came to (the view's
## last_operation: id, kind, end, findings), or {} when the call is cancelled or another
## operation's outcome stands in its place.
func _operation_end(id: int, ctx: McpToolContext) -> Dictionary:
	while not ctx.cancelled:
		var state: Variant = _parsed(String(app.call("get_operation_json")))
		if state is Dictionary:
			var running: Dictionary = (state as Dictionary).get("operation", {})
			if id == 0 or not bool(running.get("running", false)) or int(running.get("id", 0)) != id:
				var last: Variant = (state as Dictionary).get("last_operation")
				if last is Dictionary and int((last as Dictionary).get("id", 0)) == id:
					return last
				return {}
		await ctx.frames(1)
	return {}


## The findings of the request the tool just made, for a refusal's details.
func _last_problems() -> Array:
	var outcome: Variant = _parsed(String(app.call("get_outcome_json")))
	return outcome.get("findings", []) if outcome is Dictionary else []


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
