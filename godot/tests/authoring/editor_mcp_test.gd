extends GutTest

## The editor MCP (ADR 0046 S6d): the editor's endpoint over the same transport as the
## game's, driving the John Smith flow from outside. A headless editor, its service on
## an ephemeral port, a loopback client: the catalog, the state with no project, the
## refusals; then a new project, create-missing, the menu opened, a record read and
## edited through JSON, a string added, save, build, Play, the running game's STARTUP
## screen showing the edits through the game MCP, Exit ending the run, the run section
## saying so. S9k2: windows copied across screens and menu files, the selection duplicated,
## windows arranged through the preview. S9m: the menu reads and batches: the tree, a
## batch by label in one undo step, a list replaced, the findings, and their refusals.
## S11b: the Problems query (severities, a page, only the fixable, grouped by kind), and a
## problem's fix is a request passed back as it is. S11g: an import planned with the files
## it needs, its rows passed back to import_files. S13 A1: a build is the editor's
## operation, read mid-way through editor_state while it steps frame by frame, and a
## build request with wait joins it and waits for it to land (S26 K2: editor_build and
## editor_play are gone, Build and Play are editor_request's, the run editor_state's).
## S13 A4: the request table on the wire (the kind enum, the fields each kind takes, the
## game install's words). S13 A5: one read seam and
## one write seam: editor_state answers the view by section, editor_query every other read
## (the files, a document and its records, the problems, the asset graph, a menu's tree,
## findings and render, the import plan, the output, the operation, the catalog), each list a
## page; editor_request raises every change, a record batch in the batch form (records by
## identity or by the label an earlier add gave, kinds by token, a list replaced), its outcome
## naming what it added and what each label made; the tool list and the request enum come from
## `editor_query catalog`, and editor_document, editor_graph, editor_problems and editor_menu
## are gone. S13 V7: one editor_viewport tool for every viewport, its reads the viewport query
## (state, items, hit, notes, render: the catalog's op tokens) and its writes requests (options,
## camera and seek a set_viewport, seek the clock alone with no document named, drag and command an
## edit_in_viewport), a menu's through the John Smith flow and a model's camera and clock;
## editor_menu_preview and editor_model_preview are gone.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const McpTestClient := preload("res://tests/mcp/mcp_test_client.gd")
const CATALOG: Array[String] = [
	"editor_state", "editor_request", "editor_query", "editor_viewport", "editor_screenshot", "editor_logs",
]

var _dirs: Array[String] = []
var _app: Node = null
var _service: EditorMcpService = null
var _client: RefCounted = null


func before_each() -> void:
	var seam: RefCounted = await EditorFixture.boot(self, "mcp", _dirs)
	if seam == null:
		return
	_app = seam.app
	_service = add_child_autofree(EditorMcpService.new())
	assert_eq(_service.setup(_app, 0), OK)
	assert_true(_service.is_running())
	_client = McpTestClient.new()
	assert_true(await _client.connect_to(get_tree(), _service.get_port()))
	assert_not_null(await _client.initialize(get_tree()))


func after_each() -> void:
	if _client != null:
		_client.close()
		_client = null
	if is_instance_valid(_app):
		_app.request_json(JSON.stringify({"kind": "stop_play"}))
		var deadline := Time.get_ticks_msec() + 10000
		while _play_state() != "stopped" and Time.get_ticks_msec() < deadline:
			await get_tree().create_timer(0.05).timeout
			_app.pump()
	_app = null
	_service = null
	McpLogHub.instance = null
	await EditorFixture.teardown(self, _dirs)


## Play's state straight from the app's wire seam (the client may be gone): "stopped" when none.
func _play_state() -> String:
	var answer: Variant = JSON.parse_string(String(_app.query_json("state", JSON.stringify({"sections": ["run"]}))))
	if not (answer is Dictionary):
		return "stopped"
	return String((answer as Dictionary).get("run", {}).get("state", "stopped"))


## One tools/call: the structuredContent, or {"_error": text} when the tool refused.
## An editor_request waits for the operation it starts (its `wait`, S13 A3: an Open, a refresh, an
## import, a rename) unless the call says otherwise, as a test that reads the operation mid-way does.
func _call(name: String, args := {}) -> Dictionary:
	if name == "editor_request" and not args.has("wait"):
		args = args.duplicate()
		args["wait"] = true
	var envelope: Variant = await _client.call_tool(get_tree(), name, args)
	assert_true(envelope is Dictionary, "%s answered" % name)
	if not (envelope is Dictionary):
		return {}
	var result: Dictionary = envelope.get("result", {})
	if bool(result.get("isError", false)):
		var text := ""
		for block in result.get("content", []):
			if String(block.get("type", "")) == "text":
				text += String(block["text"]) + "\n"
		return {"_error": text}
	return result.get("structuredContent", {})


## One editor_query: the answer, or {"_error": text} when the query refused its params.
func _query(name: String, args := {}) -> Dictionary:
	var params := {"query": name}
	params.merge(args)
	return await _call("editor_query", params)


## The state's sections by name.
func _state(sections: Array) -> Dictionary:
	return await _call("editor_state", {"sections": sections})


## A build asked over editor_request and awaited (wait): the operation query's build, with the operation
## it waited on and ok only when that ended done; the request's own answer when it was not done (a refusal,
## the unsaved-changes prompt: outcome.unsaved_prompt).
func _build() -> Dictionary:
	var answer := await _call("editor_request", {"kind": "build"})
	if answer.has("_error") or not bool(answer.get("outcome", {}).get("done", false)):
		return answer
	var build: Dictionary = (await _query("operation")).get("build", {})
	build["operation"] = _ended(answer)
	build["ok"] = bool(build.get("ok", false)) and String(_ended(answer).get("end", "")) == "done"
	return build


## Play asked over editor_request and awaited (its build lands, the game starts): the run section after it;
## the request's own answer when it was not done.
func _play(fields := {}) -> Dictionary:
	var request := {"kind": "play"}
	request.merge(fields)
	var answer := await _call("editor_request", request)
	if answer.has("_error") or not bool(answer.get("outcome", {}).get("done", false)):
		return answer
	return await _run()


## The run section (Play's state, pid, port, exit).
func _run() -> Dictionary:
	return (await _state(["run"])).get("run", {})


## What the operation a request started came to, when the call waited for it ({} for none).
static func _ended(answer: Dictionary) -> Dictionary:
	var ended: Variant = answer.get("operation", {})
	return ended if ended is Dictionary else {}


## Whether a request's answer says it read and was done, and the operation it started ended done.
static func _done(answer: Dictionary) -> bool:
	var ended := _ended(answer)
	return bool(answer.get("ok", false)) and bool(answer.get("outcome", {}).get("done", false)) \
			and (ended.is_empty() or String(ended.get("end", "")) == "done")


## Whether a request's answer names a finding of `code` (it read, and the session reported it:
## the request's, or the operation's it started).
static func _found(answer: Dictionary, code: String) -> bool:
	for finding: Variant in answer.get("outcome", {}).get("findings", []) + _ended(answer).get("findings", []):
		if finding is Dictionary and String((finding as Dictionary).get("code", "")) == code:
			return true
	return false


## One edit_record batch in the batch form (records by identity, or by the label an earlier add
## or duplicate of the batch gave with `as`), on the document `path` names ("" the active one):
## the request's answer, its outcome naming the records added and what each label made.
func _edit(edits: Array, path := "") -> Dictionary:
	var request := {"kind": "edit_record", "edits": edits}
	if not path.is_empty():
		request["path"] = path
	return await _call("editor_request", request)


## One field of a record of the active document set: the request's answer.
func _set_field(id: int, field: String, value: Variant) -> Dictionary:
	return await _edit([{"op": "set", "id": id, "field": field, "value": value}])


## A record of `kind` added to the active document, into `parent` (a record, or a row's own
## identity; 0 a new row) at `position` (-1 the end): its identity, 0 when it was refused.
func _add(kind: String, parent := 0, position := -1) -> int:
	var edit := {"op": "add", "kind": kind}
	if parent > 0:
		edit["parent"] = parent
	if position >= 0:
		edit["position"] = position
	var answer := await _edit([edit])
	var added: Array = answer.get("outcome", {}).get("added", [])
	return int(added[0]) if _done(answer) and not added.is_empty() else 0


## A record of the active document by its identity ({"_error": ...} when it has none).
func _record(id: int) -> Dictionary:
	return await _query("record", {"id": id})


## The record the active document defines `symbol` by (in `scope`): its identity, 0 for none.
func _find(symbol: String, scope := "") -> int:
	var args := {"symbol": symbol}
	if not scope.is_empty():
		args["scope"] = scope
	return int((await _query("record", args)).get("id", 0))


## A field's value as its record reads it (null for an optional field the file leaves out).
func _value(id: int, field: String) -> Variant:
	for entry: Variant in (await _record(id)).get("fields", []):
		if entry is Dictionary and String((entry as Dictionary).get("id", "")) == field:
			if bool(entry.get("optional", false)) and not bool(entry.get("present", true)):
				return null
			return entry.get("value")
	return null


## A document opened (and made the active one): the document query's answer.
func _open(path: String) -> Dictionary:
	await _call("editor_request", {"kind": "open_document", "path": path})
	return await _query("document", {"path": path})


## A record selected ("replace", "add" or "toggle", by its address as the record query gives
## it): {ok, records} after, as the selection section says.
func _select(id: int, mode := "replace") -> Dictionary:
	var record := await _record(id)
	if record.has("_error"):
		return record
	var request := {"kind": "select_record", "path": String(record.get("document", "")), "mode": mode,
			"address": {"row": int(record.get("row", 0)), "kind": int(record.get("kind", 0)),
					"child": int(record.get("child", 0))}}
	var answer := await _call("editor_request", request)
	var selection: Dictionary = (await _state(["selection"])).get("selection", {})
	return {"ok": _done(answer), "records": selection.get("records", []), "primary": selection.get("primary", {})}


## A record's address as the record query gives it: {row, kind, child}.
func _address_of(id: int) -> Dictionary:
	var record := await _record(id)
	return {"row": int(record.get("row", 0)), "kind": int(record.get("kind", 0)), "child": int(record.get("child", 0))}


## A request on the active document (copy, cut, duplicate, undo, redo, end_edit, ...): its answer.
func _ask(kind: String, fields := {}) -> Dictionary:
	var request := {"kind": kind}
	request.merge(fields)
	return await _call("editor_request", request)


## Every output line held, oldest first, page after page.
func _output() -> String:
	var lines := PackedStringArray()
	var cursor := 0
	for _guard in 100:
		var page := await _query("output", {"cursor": cursor, "limit": 200})
		for line: Variant in page.get("lines", []):
			lines.append(String(line))
		var next := int(page.get("next_cursor", cursor))
		if next == cursor or next >= int(page.get("next", next)):
			break
		cursor = next
	return "\n".join(lines)


## A build raised through editor_request returns at once with its operation, which steps a
## small budget a frame: editor_state reads it mid-way (running, its id, bytes done short of the
## total, cancellable, holding the files), a build request with wait joins it and returns once it lands, and
## the state then shows what it came to. The main thread is never held: the state answers while
## the build runs.
func test_build_is_an_operation_the_state_reads_mid_way() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova build operation %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var made := await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Operation"})
	assert_true(bool(made.get("ok", false)), str(made))
	assert_true(bool((await _create_missing()).get("ok", false)))
	_app.call("set_poll_budget", 0, 8192) # a step of 8 KiB a frame
	var raised := await _call("editor_request", {"kind": "build", "wait": false})
	var outcome: Dictionary = raised.get("outcome", {})
	assert_true(bool(outcome.get("done", false)), str(raised))
	var id := int(outcome.get("operation", 0))
	assert_gt(id, 0, str(raised))
	var state := await _state(["operation"])
	var operation: Dictionary = state.get("operation", {}).get("operation", {})
	assert_true(bool(operation.get("running", false)), str(operation))
	assert_eq(int(operation.get("id", 0)), id)
	assert_eq(String(operation.get("kind", "")), "build")
	assert_eq(String(operation.get("unit", "")), "bytes")
	assert_lt(int(operation.get("done", 0)), int(operation.get("total", 0)), str(operation))
	assert_true(bool(operation.get("cancellable", false)))
	assert_eq(operation.get("reads", []), ["files"])
	assert_eq(operation.get("writes", []), ["slot"])
	# The operation query says the same while it runs.
	var queried := await _query("operation")
	assert_eq(int(queried.get("operation", {}).get("id", 0)), id, str(queried))
	var saved := await _call("editor_request", {"kind": "save_all"})
	assert_false(bool(saved.get("outcome", {}).get("done", true)), "a save waits for the build: " + str(saved))
	var built := await _build()
	assert_true(bool(built.get("ok", false)), str(built))
	assert_eq(int(built.get("operation", {}).get("id", 0)), id, "the build request joined the build that ran")
	assert_eq(String(built.get("operation", {}).get("end", "")), "done")
	state = await _state(["operation"])
	assert_false(bool(state.get("operation", {}).get("operation", {}).get("running", true)))
	assert_eq(int(state.get("operation", {}).get("last_operation", {}).get("id", 0)), id)
	assert_eq(String(state.get("operation", {}).get("last_operation", {}).get("end", "")), "done")
	_app.call("set_poll_budget", 10, 1048576)


## Every missing required file created, as the editor's Create all asks: create_missing
## names the roles of the checklist's unmet rows (it makes nothing without names).
func _create_missing() -> Dictionary:
	var roles: Array[String] = []
	for row: Variant in (await _state(["requirements"])).get("requirements", {}).get("rows", []):
		if bool((row as Dictionary).get("required", false)) and String((row as Dictionary).get("state", "")) != "present":
			roles.append(String((row as Dictionary)["role"]))
	return await _call("editor_request", {"kind": "create_missing", "roles": roles})


## An open document's entry in an editor_state documents section ({} when it is not open).
static func _document_state(state: Dictionary, path: String) -> Dictionary:
	for document: Variant in state.get("documents", {}).get("open", []):
		if document is Dictionary and String((document as Dictionary).get("path", "")) == path:
			return document
	return {}


## A widget's absolute rect in an editor_viewport answer's items (all zero when it is not there).
func _rect_of(rects: Dictionary, name: String) -> Array:
	for widget: Variant in rects.get("items", []):
		if widget is Dictionary and String((widget as Dictionary).get("name", "")) == name:
			return (widget as Dictionary).get("rect", [0, 0, 0, 0])
	return [0, 0, 0, 0]


## S26 C1: an answer comes back whole, the session's JSON as it made it: the game MCP's sanitizer cut a list at
## 200 entries with a string in its place, a depth past 24 and a string past 8192 characters. The catalog's
## finding codes run past 200, every one an object.
func test_answers_come_back_whole() -> void:
	if _client == null:
		return
	var codes: Array = (await _query("catalog")).get("finding_codes", [])
	assert_gt(codes.size(), 200, "the catalog lists every finding code")
	for code: Variant in codes:
		assert_true(code is Dictionary, "no sentinel in the list: %s" % str(code))


# S26 C2: editor_request's wait is held below the tool's own budget, which cut a longer one with a bare error and
# cancelled the handler: the schema's maximum and default, the description naming it, the handler's clamp the same,
# and a wait asked far past it answered as any other, never a budget error.
func test_request_waits_within_the_tool_budget() -> void:
	if _client == null:
		return
	assert_lt(EditorMcpCatalog.REQUEST_WAIT_MAX_MS, EditorMcpCatalog.TOOL_BUDGET_MS)
	assert_eq(EditorMcpTools.REQUEST_WAIT_MS, EditorMcpCatalog.REQUEST_WAIT_MAX_MS)
	var listed: Variant = await _client.rpc(get_tree(), "tools/list")
	var wait_ms := {}
	var described := ""
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		if String(tool["name"]) == "editor_request":
			wait_ms = tool.get("inputSchema", {}).get("properties", {}).get("wait_ms", {})
			described = String(tool.get("description", ""))
	assert_eq(int(wait_ms.get("maximum", 0)), EditorMcpCatalog.REQUEST_WAIT_MAX_MS)
	assert_true(String(wait_ms.get("description", "")).contains(str(EditorMcpCatalog.REQUEST_WAIT_MAX_MS)))
	assert_true(described.contains("(%d when left out, and at most)" % EditorMcpCatalog.REQUEST_WAIT_MAX_MS), described)
	var answer := await _call("editor_request", {"kind": "clear_output", "wait": true, "wait_ms": 100_000_000})
	assert_false(answer.has("_error"), str(answer))
	assert_false(bool(answer.get("timed_out", false)), str(answer))


## With no project: the tool list, the state, and what the tools refuse (the pickers need a person, an unknown
## kind, a screenshot headless, a viewport with no document, Build and Play with no project), the log saying the
## endpoint listens. The session's own refusals (a member no kind takes, a query with no document, an unknown
## query) are session_json_test's test_request_round_trip and editor_query_test's test_refusals.
func test_catalog_state_and_refusals_without_a_project() -> void:
	if _client == null:
		return
	var listed: Variant = await _client.rpc(get_tree(), "tools/list")
	assert_true(listed is Dictionary)
	var names: Array[String] = []
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		names.append(String(tool["name"]))
	assert_eq(names, CATALOG)
	var state := await _call("editor_state")
	assert_false(bool(state.get("project", {}).get("open", true)))
	assert_eq(String(state.get("run", {}).get("state", "")), "stopped")
	assert_eq(String(state.get("status", {}).get("status", "")), "No project open.")
	assert_true((await _call("editor_request", {"kind": "pick_directory"})).get("_error", "").contains("person"),
			"the pickers need a person")
	assert_true((await _call("editor_request", {"kind": "nope"})).get("_error", "").contains("nope"))
	assert_true(str((await _call("editor_request", {"kind": "build"})).get("outcome", {})).contains("project.none"))
	assert_true(str((await _call("editor_request", {"kind": "play"})).get("outcome", {})).contains("project.none"))
	assert_true((await _call("editor_screenshot")).get("_error", "").contains("headless"))
	assert_true((await _call("editor_viewport", {"op": "state"})).get("_error", "").contains("no document is open"),
			"no project, no viewport")
	var logs := await _call("editor_logs")
	var texts: Array[String] = []
	for entry in logs.get("entries", []):
		texts.append(String(entry.get("text", "")))
	assert_true("\n".join(texts).contains("listening"), str(logs))


## S13 A4, the request table on the wire: editor_request's kind enum is every token the table
## serves (S13 A5: the catalog query's request kinds but those a person answers, the pickers), the
## query enum the catalog's queries, editor_viewport's ops, kinds and members the viewport query's
## (S13 V7), the edits' schema the session's batch table (S13 D9), set_workspace's workspace the
## catalog's workspace table; one refusal read through the tool (a member its kind does not take).
## How the session reads a request (each kind's members, the retired ones, the retail token, the game
## install's settings and the run and import sections' words) is session_json_test's test_tokens,
## test_request_round_trip, test_request_table_samples, test_settings_json and test_game_install_keys; an
## import from an install holding no archives is import_plan_test's test_plan_game_install.
func test_request_table_on_the_wire() -> void:
	if _client == null:
		return
	var listed: Variant = await _client.rpc(get_tree(), "tools/list")
	var kinds: Array = []
	var queries: Array = []
	var edit: Dictionary = {}
	var workspace: Dictionary = {}
	var viewport: Dictionary = {}
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		if String(tool["name"]) == "editor_request":
			kinds = tool["inputSchema"]["properties"]["kind"].get("enum", [])
			edit = tool["inputSchema"]["properties"]["edits"].get("items", {})
			workspace = tool["inputSchema"]["properties"].get("workspace", {})
		if String(tool["name"]) == "editor_query":
			queries = tool["inputSchema"]["properties"]["query"].get("enum", [])
		if String(tool["name"]) == "editor_viewport":
			viewport = tool
	var catalog := await _query("catalog")
	var served: Array = []
	for row: Variant in catalog.get("requests", []):
		if String((row as Dictionary).get("served_by", "")) != "person":
			served.append(String((row as Dictionary).get("kind", "")))
	assert_eq(kinds, served, "the kind enum is the tokens the request table serves, from the catalog")
	var named: Array = []
	for row: Variant in catalog.get("queries", []):
		named.append(String((row as Dictionary).get("name", "")))
	assert_eq(queries, named, "the query enum is the catalog's queries")
	# S13 V7: editor_viewport, made from the catalog: its op the viewport query's reads (the catalog's
	# op tokens) and the writes (set_viewport's and edit_in_viewport's), the query's params but op flat
	# beside it (kind's tokens its enum), the objects the writes carry.
	assert_true(served.has("set_viewport") and served.has("edit_in_viewport") and named.has("viewport"))
	var reads: Array = []
	var viewport_kinds: Array = []
	for row: Variant in catalog.get("queries", []):
		if String((row as Dictionary).get("name", "")) == "viewport":
			for param: Variant in (row as Dictionary).get("params", []):
				if String((param as Dictionary).get("name", "")) == "op":
					reads = (param as Dictionary).get("enum", [])
				elif String((param as Dictionary).get("name", "")) == "kind":
					viewport_kinds = (param as Dictionary).get("enum", [])
	assert_eq(reads, ["state", "items", "hit", "box", "notes", "render", "palette"], "the catalog lists the query's ops")
	assert_true(viewport_kinds.has("menu") and viewport_kinds.has("model"), str(viewport_kinds))
	var properties: Dictionary = viewport.get("inputSchema", {}).get("properties", {})
	assert_eq(properties.get("op", {}).get("enum", []),
			reads + ["options", "camera", "try", "click", "key", "seek", "drag", "command", "drop"])
	for key in ["path", "kind", "x", "y", "x2", "y2", "row", "text", "offset", "limit", "options", "camera", "clock",
			"device", "try", "click", "key", "drag", "command", "drop"]:
		assert_true(properties.has(key), "editor_viewport takes %s" % key)
	assert_eq(properties.get("kind", {}).get("enum", []), viewport_kinds, "the viewport kinds' tokens, the catalog's")
	assert_eq(String(properties.get("x", {}).get("type", "")), "number", "a point is a number")
	assert_eq(int(properties.get("limit", {}).get("maximum", 0)), int(catalog.get("page_max", -1)))
	var description := String(viewport.get("description", ""))
	assert_true(description.contains("set_viewport") and description.contains("edit_in_viewport"), description)
	var batch: Dictionary = catalog.get("batch", {})
	var ops: Array = []
	for row: Variant in batch.get("ops", []):
		ops.append(String((row as Dictionary).get("op", "")))
	var members: Dictionary = edit.get("properties", {})
	assert_eq(members.get("op", {}).get("enum", []), ops, "the edit's ops are the batch table's")
	assert_true(ops.has("apply") and ops.has("replace_list"), str(ops))
	assert_eq(members.size(), (batch.get("members", []) as Array).size(), "a property per member of the table")
	assert_eq(members.get("payload", {}).get("enum", []), ["text.span"], str(members.get("payload")))
	assert_eq(int(members.get("line", {}).get("minimum", 0)), 1, str(members.get("line")))
	assert_true(String(members.get("text", {}).get("description", "")).contains("spans"), str(members.get("text")))
	# The MCP gaps lane: set_workspace's workspace is the catalog's workspace table, a property per part (its
	# members, each typed) and focus the windows it brings forward.
	var parts: Dictionary = workspace.get("properties", {})
	var table: Dictionary = catalog.get("workspace", {})
	assert_eq(parts.size(), (table.get("parts", []) as Array).size() + 1, "a property per part, and focus")
	assert_eq(String(parts.get("card", {}).get("properties", {}).get("path", {}).get("type", "")), "string", str(parts.get("card")))
	assert_eq(String(parts.get("build_result", {}).get("properties", {}).get("open", {}).get("type", "")), "boolean")
	assert_eq(parts.get("focus", {}).get("enum", []), table.get("focus", []))
	# The import checks are whole numbers, as import_preview's rows say them; a text carries the
	# longest its window's field holds.
	var import_members: Dictionary = parts.get("import", {}).get("properties", {})
	assert_eq(String(import_members.get("check", {}).get("items", {}).get("type", "")), "integer", str(import_members.get("check")))
	assert_eq(int(parts.get("problems", {}).get("properties", {}).get("text", {}).get("maxLength", 0)), 127,
			str(parts.get("problems", {}).get("properties", {}).get("text")))
	assert_true(served.has("set_workspace") and served.has("play_sound") and served.has("stop_sound") and
			served.has("apply_confirmation"))
	assert_false(served.has("pick_directory") or served.has("pick_file"), "the pickers need a person")
	assert_true(served.has("preview_install_import") and not served.has("preview_retail_import"))
	assert_true(String((await _call("editor_request", {"kind": "build", "path": "x"})).get("_error", "")).contains(
			"build takes no \"path\" (it takes out_dir, rehash, report)"))


## Play's settings through the endpoint: with no project they are refused (project.none); a new project plays in
## the OpenNova runtime and saves first, apply_project_settings sets its play_mode and save_before_play, the run
## and preferences sections say them and its .opennova/local.json keeps them. play takes a play_mode for one Play
## alone (the catalog's modes), a mode no Play has refused. That each project keeps its own, never the editor's
## settings file, is editor_session_test's test_play_settings_per_project.
func test_play_settings_are_each_projects() -> void:
	if _client == null:
		return
	var nothing := await _call("editor_request", {"kind": "apply_project_settings", "settings": {"play_mode": "strict"}})
	assert_true(str(nothing.get("outcome", {}).get("findings", [])).contains("project.none"), str(nothing))
	var root := OS.get_cache_dir().path_join("opennova play settings %d" % Time.get_ticks_usec())
	_dirs.append(root)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": root, "title": "Strict one"})).get("ok", false)))
	var fresh: Dictionary = (await _state(["run", "preferences"]))
	assert_eq(String(fresh.get("run", {}).get("play_mode", "")), "runtime", "a project never set plays in OpenNova")
	assert_true(bool(fresh.get("preferences", {}).get("save_before_play", false)), str(fresh))
	var applied := await _call("editor_request", {"kind": "apply_project_settings",
			"settings": {"play_mode": "strict", "save_before_play": false}})
	assert_true(bool(applied.get("outcome", {}).get("done", false)), str(applied))
	var set_state: Dictionary = (await _state(["run", "preferences"]))
	assert_eq(String(set_state.get("run", {}).get("play_mode", "")), "strict", str(set_state))
	assert_eq(String(set_state.get("preferences", {}).get("play_mode", "")), "strict", str(set_state))
	assert_false(bool(set_state.get("run", {}).get("save_before_play", true)), str(set_state))
	var local := FileAccess.get_file_as_string(root.path_join(".opennova/local.json"))
	assert_true(local.contains("\"play_mode\": \"strict\"") and local.contains("\"save_before_play\": false"), local)
	var listed: Variant = await _client.rpc(get_tree(), "tools/list")
	var request_tool := {}
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		if String(tool["name"]) == "editor_request":
			request_tool = tool
	assert_eq(request_tool.get("inputSchema", {}).get("properties", {}).get("play_mode", {}).get("enum", []),
			["runtime", "strict"])
	assert_true(request_tool.get("inputSchema", {}).get("properties", {}).has("save_before_play"), str(request_tool))
	var refused := await _call("editor_request", {"kind": "play", "play_mode": "retail"})
	assert_true(String(refused.get("_error", "")).contains("play_mode"), str(refused))
	assert_eq(String((await _state(["run"])).get("run", {}).get("state", "")), "stopped")


## The previews' background is an editor preference the MCP reads and sets: the preferences section says it
## (grey before any is set), set_preview_background sets it (its catalog enum the four tokens), the editor's
## settings file keeps it, and a word none has is refused naming the field, nothing changed. No project is
## needed: it is the editor's, not a project's. Each of the four read back as written is session_json_test's
## test_preview_background_json.
func test_the_preview_background_through_the_editor_mcp() -> void:
	if _client == null:
		return
	assert_eq(String((await _state(["preferences"])).get("preferences", {}).get("preview_background", "")), "grey")
	var listed: Variant = await _client.rpc(get_tree(), "tools/list")
	var request_tool := {}
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		if String(tool["name"]) == "editor_request":
			request_tool = tool
	var schema: Dictionary = request_tool.get("inputSchema", {}).get("properties", {})
	assert_eq(schema.get("preview_background", {}).get("enum", []), ["dark", "grey", "light", "checker"], str(schema.keys()))
	var answer := await _call("editor_request", {"kind": "set_preview_background", "preview_background": "light"})
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
	assert_eq(String((await _state(["preferences"])).get("preferences", {}).get("preview_background", "")), "light")
	var settings_text := FileAccess.get_file_as_string(String(_app.get("settings_path")))
	assert_true(settings_text.contains("\"preview_background\": \"light\""), settings_text)
	var refused := await _call("editor_request", {"kind": "set_preview_background", "preview_background": "purple"})
	assert_true(String(refused.get("_error", "")).contains("preview_background"), str(refused))
	assert_eq(String((await _state(["preferences"])).get("preferences", {}).get("preview_background", "")), "light")


func test_john_smith_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova john smith mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var made := await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "John Smith"})
	assert_true(bool(made.get("ok", false)), str(made))
	var state := await _call("editor_state")
	assert_true(bool(state["project"]["open"]))
	assert_eq(String(state["project"]["title"]), "John Smith")
	assert_gt(int(state["requirements"]["total"]), 0)
	assert_eq(int(state["requirements"]["missing"]), int(state["requirements"]["total"]))
	assert_eq(int(state["problem_counts"]["errors"]), int(state["requirements"]["total"]))
	var problems := await _query("problems", {"severities": ["error"]})
	assert_eq(int(problems["shown"]), int(state["requirements"]["total"]))
	assert_eq(int(problems["total"]), int(state["problem_counts"]["count"]))
	assert_eq(int(problems["counts"]["errors"]), int(state["requirements"]["total"]))
	var page := await _query("problems", {"severities": ["error"], "offset": 1, "limit": 2})
	assert_eq(page["problems"].size(), 2)
	assert_eq(int(page["shown"]), int(problems["shown"]))
	assert_eq(String(page["problems"][0]["message"]), String(problems["problems"][1]["message"]))
	assert_true((await _query("problems", {"group": "folder"})).get("_error", "").contains("folder"),
			"a query the editor does not take is refused")
	# Only the fixable, grouped by kind: the required files, each a finding of the project (no
	# file of it), with its fixes as requests to pass back as they are.
	var fixable := await _query("problems", {"severities": ["error"], "fixable": true, "group": "kind"})
	assert_eq(int(fixable["shown"]), int(state["requirements"]["total"]), str(fixable.get("groups", [])))
	assert_eq(fixable.get("groups", []).size(), 1)
	assert_eq(String(fixable["groups"][0]["title"]), "Required files")
	var first: Dictionary = fixable["problems"][0]
	assert_false(first.has("asset"), "a required file the project lacks names no file of it")
	assert_eq(String(first["group"]), "requirement")
	assert_false(String(first["role"]).is_empty())
	var create: Dictionary = first["fixes"][0]
	assert_true(bool(create["bulk"]))
	assert_eq(String(create["request"]["kind"]), "create_missing")
	var created := await _call("editor_request", create["request"])
	assert_true(bool(created.get("outcome", {}).get("done", false)), str(created))
	state = await _call("editor_state")
	assert_eq(int(state["requirements"]["missing"]), int(state["requirements"]["total"]) - 1)

	var none := await _call("editor_request", {"kind": "create_missing"})
	assert_true(bool(none.get("outcome", {}).get("done", false)), "no names: nothing to make")
	assert_true(bool((await _create_missing()).get("ok", false)))
	state = await _call("editor_state")
	assert_eq(int(state["requirements"]["missing"]), 0)
	# Grouped by file, a page names only its own groups (a project can have a group per file):
	# one row, one group, and group_count says how many there are in all.
	var by_file := await _query("problems", {"group": "file", "limit": 1})
	assert_eq(by_file.get("problems", []).size(), 1, str(by_file))
	assert_eq(by_file.get("groups", []).size(), 1, str(by_file))
	assert_gt(int(by_file.get("group_count", 0)), 1, str(by_file))
	assert_eq(String(by_file["problems"][0]["group"]), String(by_file["groups"][0]["key"]))
	var listed := await _query("files", {"limit": 200})
	var editable: Array[String] = []
	for file in listed.get("files", []):
		if bool(file.get("editable", false)):
			editable.append(String(file["name"]))
	assert_has(editable, "main.mnu")
	assert_has(editable, "gametext.bin")

	# The menu: rows, a record through the schema, edits by id and through the request.
	var menu := await _open("main.mnu")
	assert_eq(String(menu.get("kind", "")), "menu", str(menu))
	var rows := await _query("document")
	assert_eq(String(rows["rows"][0]["name"]), "STARTUP")
	assert_eq(int(rows["row_count"]), 1)
	assert_eq(int(rows["count"]), 1)
	var screen := int(rows["rows"][0]["id"])
	assert_true((await _query("document", {"offset": 1})).get("rows", [1]).is_empty(),
			"a page past the rows is empty")
	# The wire carries whole numbers as integers (Godot's parser reads floats).
	_client.close()
	_client = McpTestClient.new()
	assert_true(await _client.connect_to(get_tree(), _service.get_port()))
	assert_not_null(await _client.initialize(get_tree()))
	await _client.call_tool(get_tree(), "editor_query", {"query": "document"})
	var wire: String = _client.last_body.get_string_from_utf8()
	assert_true(wire.contains("\"row_count\":1,") or wire.contains("\"row_count\": 1,") or wire.contains("\"row_count\":1}"), wire.left(400))
	assert_false(wire.contains("\"row_count\":1.0"), "ids and counts are integers on the wire")
	var title := await _find("TITLE")
	assert_gt(title, 0)
	var record := await _record(title)
	var fields := {}
	for field in record.get("fields", []):
		fields[String(field["id"])] = field
	assert_eq(String(fields["string.value"]["value"]), "John Smith")
	assert_eq(String(fields["font.name"]["reference"]), "font")
	assert_true(_done(await _set_field(title, "string.value", "John Smith's Game")))
	assert_eq(String(await _value(title, "string.value")), "John Smith's Game")
	# Changed since the save: the record says so with what the file holds; revert gives it
	# back (the Inspector's Revert to saved), and a second revert has nothing to go back to.
	var changed := await _record(title)
	assert_eq(String(changed.get("change", "")), "changed", str(changed))
	for field in changed.get("fields", []):
		if String(field["id"]) == "string.value":
			assert_true(bool(field.get("changed", false)), str(field))
			assert_eq(String(field.get("saved", "")), "John Smith", str(field))
	var reverted := await _call("editor_request", {"kind": "revert_to_saved", "edits": [{"id": title, "field": "string.value"}]})
	assert_true(_done(reverted), str(reverted))
	assert_eq(String(await _value(title, "string.value")), "John Smith")
	var again := await _call("editor_request", {"kind": "revert_to_saved", "edits": [{"id": title, "field": "string.value"}]})
	assert_true(bool(again.get("ok", false)) and not _done(again), "nothing left to revert: " + str(again))
	assert_true(_done(await _set_field(title, "string.value", "John Smith's Game")))
	assert_eq(String(await _value(title, "string.value")), "John Smith's Game")
	# The screen holds its root windows (a second one added and removed); a window goes
	# inside a window, at any depth.
	var main := int(rows["rows"][0]["collections"][0]["records"][0]["id"])
	assert_eq(String(rows["rows"][0]["collections"][0]["records"][0]["name"]), "MAIN")
	var second_root := await _add("window", screen)
	assert_gt(second_root, 0, "a second root window")
	assert_true(_done(await _edit([{"op": "remove", "id": second_root}])))
	assert_false(_done(await _edit([{"op": "remove", "id": main}])), "a screen keeps one root")
	var later := await _add("window", main)
	assert_gt(later, 0)
	var later_record := await _record(later)
	assert_eq(String(later_record.get("path", "")), "STARTUP/MAIN/WINDOW1", str(later_record))
	assert_eq(int(later_record.get("owner", {}).get("child", 0)), main)
	assert_eq(int(later_record.get("index", -1)), 2)
	for pair in [["name", "LATER"], ["type", "button"], ["string.value", "Later"], ["position.left", 340],
			["position.top", 430], ["position.right", 460]]:
		var one := await _set_field(later, pair[0], pair[1])
		assert_true(_done(one), str(one))
	var selected := await _call("editor_request", {
		"kind": "select_record", "path": "main.mnu", "address": {"row": screen, "kind": 1, "child": later},
	})
	assert_true(bool(selected.get("ok", false)), str(selected))
	state = await _call("editor_state")
	assert_eq(int(state["selection"]["primary"]["child"]), later)
	assert_true(bool(state["documents"]["open"][0]["dirty"]))
	# The viewport (headless, editor_viewport): the screen as the game would draw it, LATER where it
	# was put and the title's new text; the game's hit test at LATER's centre finds it; the options
	# hold it under the mouse (a set_viewport, the state after it its answer's viewport).
	var preview := await _call("editor_viewport", {"op": "state"})
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_eq(String(preview.get("kind", "")), "menu", str(preview))
	assert_eq(String(preview.get("body", {}).get("screen", {}).get("name", "")), "STARTUP")
	assert_gt(int(preview.get("count", 0)), 3)
	assert_gt(int(preview.get("view_revision", 0)), 0, "stamped by the viewport query")
	var rects := await _call("editor_viewport", {"op": "items", "limit": 200})
	var by_name := {}
	for widget: Variant in rects.get("items", []):
		by_name[String(widget["name"])] = widget
	assert_eq(String(by_name.get("TITLE", {}).get("text", "")), "John Smith's Game", str(rects))
	var later_rect: Array = by_name.get("LATER", {}).get("rect", [])
	assert_eq(later_rect.size(), 4, str(rects))
	if later_rect.size() == 4:
		assert_eq(int(later_rect[0]), 340)
		var hit := await _call("editor_viewport", {"op": "hit",
				"x": (float(later_rect[0]) + float(later_rect[2])) / 2.0,
				"y": (float(later_rect[1]) + float(later_rect[3])) / 2.0})
		assert_eq(int(hit.get("id", 0)), later, str(hit))
	var held := await _call("editor_viewport", {"op": "options", "options": {"force_id": later, "force_state": "mouseover"}})
	assert_true(_done(held), str(held))
	assert_eq(int(held.get("viewport", {}).get("options", {}).get("force_id", 0)), later, str(held))
	assert_eq(String(held.get("viewport", {}).get("options", {}).get("force_state", "")), "mouseover", str(held))
	# LATER dragged by its move handle onto the grid, then a unit more with no snap in two drags of
	# one gesture (the first's answer names it; the second, which moves nothing, ends it): two undo
	# steps.
	var dragged := await _call("editor_viewport", {"op": "drag",
			"drag": {"id": later, "handle": "move", "by": [13, 5], "snap": 1}})
	assert_true(_done(dragged), str(dragged))
	assert_gt(int(dragged.get("outcome", {}).get("gesture", 0)), 0, str(dragged))
	assert_eq(int(_rect_of(dragged.get("viewport", {}), "LATER")[0]), 352, str(dragged))
	var opened_gesture := await _call("editor_viewport", {"op": "drag",
			"drag": {"id": later, "handle": "move", "by": [1, 0], "end": false}})
	var gesture := int(opened_gesture.get("outcome", {}).get("gesture", 0))
	assert_true(_done(opened_gesture) and gesture > 0, str(opened_gesture))
	var nudged := await _call("editor_viewport", {"op": "drag",
			"drag": {"id": later, "handle": "move", "by": [0, 0], "gesture": gesture}})
	assert_true(_done(nudged), str(nudged))
	assert_eq(int(nudged.get("outcome", {}).get("gesture", 0)), gesture, "the gesture it went on with")
	assert_eq(int(_rect_of(await _call("editor_viewport", {"op": "items", "limit": 200}), "LATER")[0]), 353)
	# Refused: by the viewport as it is served (an unknown handle, a record it does not show: read,
	# not done), as the request is read (both by and to), by the tool (an op it has not, a member
	# its op does not take, the one it needs).
	var middle := await _call("editor_viewport", {"op": "drag", "drag": {"id": later, "handle": "middle", "by": [1, 1]}})
	assert_true(bool(middle.get("ok", false)) and not _done(middle), "an unknown handle: " + str(middle))
	assert_eq(String(middle.get("outcome", {}).get("findings", [{}])[0].get("code", "")), "viewport.refused", str(middle))
	var unseen := await _call("editor_viewport", {"op": "drag", "drag": {"id": 999999, "handle": "move", "by": [1, 1]}})
	assert_false(_done(unseen), "a record the viewport does not show: " + str(unseen))
	assert_true((await _call("editor_viewport", {"op": "drag",
			"drag": {"id": later, "handle": "move", "by": [1, 1], "to": [1, 1]}})).get("_error", "").contains("one of them"))
	assert_true((await _call("editor_viewport", {"op": "zoom"})).get("_error", "").contains("no op 'zoom'"))
	assert_true((await _call("editor_viewport", {"op": "options", "camera": {}})).get("_error", "").contains(
			"takes no \"camera\""))
	assert_true((await _call("editor_viewport", {"op": "drag"})).get("_error", "").contains("needs \"drag\""))
	for _step in 2:
		assert_true(_done(await _ask("undo")))
	assert_eq(int(_rect_of(await _call("editor_viewport", {"op": "items", "limit": 200}), "LATER")[0]), 340,
			"two undo steps put it back")
	assert_true((await _call("editor_viewport", {"op": "hit"})).get("_error", "").contains("needs \"x\" and \"y\""))
	assert_true((await _call("editor_viewport", {"op": "state", "x": 1})).get("_error", "").contains("takes no \"x\""))
	# The frame compiler's notes on the screen, and the render check's headless render of the same
	# screen (the viewport's render, the menu_render query's answer): the same windows.
	var notes := await _call("editor_viewport", {"op": "notes"})
	assert_eq(String(notes.get("status", "")), "ready", str(notes))
	assert_eq(int(notes.get("count", -1)), (notes.get("notes", []) as Array).size(), str(notes))
	var screen_id := int(preview.get("body", {}).get("screen", {}).get("id", 0))
	var render := await _call("editor_viewport", {"op": "render", "row": screen_id, "limit": 200})
	assert_eq(String(render.get("status", "")), "ready", str(render))
	assert_eq(int(render.get("widget_count", 0)), int(preview.get("count", -1)), str(render))
	var render_names := {}
	for widget: Variant in render.get("widgets", []):
		render_names[String(widget["name"])] = widget
	assert_true(render_names.has("LATER"), str(render))
	var checked := await _query("menu_render", {"path": String(preview.get("path", "")), "screen": screen_id, "limit": 200})
	assert_eq(int(checked.get("widget_count", -1)), int(render.get("widget_count", -2)), str(checked))
	assert_true((await _query("menu_render", {"path": "main.mnu"})).has("_error"), "render needs a screen")
	# The selection joins one record at a time; a clear leaves an edge out and a write puts it
	# back; a batch is one step.
	var joined := await _select(title, "add")
	assert_eq(joined.get("records", []).size(), 2, str(joined))
	# A marquee (S13 D7): the records named with the primary in one request, of any rows.
	var later_address := await _address_of(later)
	var title_address := await _address_of(title)
	var marquee := await _call("editor_request", {"kind": "select_record", "address": later_address,
			"records": [title_address]})
	assert_true(_done(marquee), str(marquee))
	var marqueed: Dictionary = (await _state(["selection"])).get("selection", {})
	var marqueed_records: Array = marqueed.get("records", [])
	assert_eq(marqueed_records.size(), 2, str(marqueed))
	if marqueed_records.size() == 2:
		assert_eq(int((marqueed_records[0] as Dictionary).get("child", 0)), int(title_address["child"]), str(marqueed))
		assert_eq(int((marqueed_records[1] as Dictionary).get("child", 0)), int(later_address["child"]), str(marqueed))
	assert_eq(int((marqueed.get("primary", {}) as Dictionary).get("child", 0)), int(later_address["child"]), str(marqueed))
	var cleared := await _edit([{"op": "clear", "id": later, "field": "position.right"}])
	assert_true(_done(cleared), str(cleared))
	assert_eq(await _value(later, "position.right"), null, "an unset edge reads nil")
	assert_false(_done(await _edit([{"op": "clear", "id": later, "field": "name"}])), "a name is always written")
	var written := await _edit([{"op": "write", "id": later, "field": "position.right"}])
	assert_true(_done(written), str(written))
	assert_eq(int(await _value(later, "position.right")), 460, "written again with the value it read")
	assert_false(_done(await _edit([{"op": "write", "id": later, "field": "name"}])))
	var batch := await _edit([
		{"op": "set", "id": later, "field": "position.right", "value": 470},
		{"op": "set", "id": later, "field": "position.bottom", "value": 450},
	], "main.mnu")
	assert_true(bool(batch.get("outcome", {}).get("done", false)), str(batch))
	var after_batch := await _record(later)
	var batch_fields := {}
	for field in after_batch.get("fields", []):
		batch_fields[String(field["id"])] = field
	assert_eq(int(batch_fields["position.right"]["value"]), 470)
	assert_true(bool(batch_fields["position.right"].get("present", false)))
	# The selected windows copied and pasted into MAIN (their names made unique), then undone.
	assert_true(_done(await _ask("copy")))
	assert_gt(int((await _state(["selection"])).get("selection", {}).get("clipboard_bytes", 0)), 0)
	var pasted := await _ask("paste", {"paste_at": {"parent": main}})
	assert_true(_done(pasted), str(pasted))
	assert_eq((await _state(["selection"])).get("selection", {}).get("records", []).size(), 2, str(pasted))
	assert_gt(await _find("LATER2"), 0)
	assert_true(_done(await _ask("undo")))
	assert_true((await _query("record", {"symbol": "LATER2"})).has("_error"), "the paste undone")
	assert_false(_done(await _set_field(later, "no_such_field", 1)))

	# The string table.
	var strings := await _open("gametext.bin")
	assert_eq(String(strings.get("kind", "")), "strings", str(strings))
	var section := await _add("section")
	assert_gt(section, 0)
	assert_true(_done(await _set_field(section, "name", "Menu")))
	var welcome := await _add("string", section)
	assert_gt(welcome, 0)
	assert_true(_done(await _set_field(welcome, "key", "JS_WELCOME")))
	assert_true(_done(await _set_field(welcome, "text", "Welcome, John")))

	# Dirty documents make the build wait on the unsaved prompt, which names the build and the
	# files and offers no discard; its save writes them and builds; the build lands.
	var waiting := await _build()
	assert_true(bool(waiting.get("outcome", {}).get("unsaved_prompt", false)), str(waiting))
	var prompt: Dictionary = (await _state(["dialogs"])).get("dialogs", {}).get("unsaved_prompt", {})
	assert_true(bool(prompt.get("open", false)), str(prompt))
	assert_eq(String(prompt.get("action", "")), "build")
	assert_eq(prompt.get("files", []).size(), 2, str(prompt))
	assert_false(bool(prompt.get("can_discard", true)))
	var saved := await _call("editor_request", {"kind": "resolve_unsaved", "choice": "save"})
	assert_true(bool(saved.get("outcome", {}).get("done", false)), str(saved))
	var built := await _build()
	assert_true(bool(built.get("ok", false)), str(built))
	assert_true(FileAccess.file_exists(String(built.get("dir", "")).path_join("localres.pff")), str(built))

	# Play: the game runs on the build with its own endpoint, which the game tools drive.
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--max-fps", "60"]))
	# What the checkout's Godot project holds: a source Play (the Godot binary with --path on it)
	# writes nothing there.
	var project_before := _top_level_entries(ProjectSettings.globalize_path("res://"))
	var play := await _play()
	assert_eq(String(play.get("state", "")), "running", str(play))
	var port := int(play.get("mcp_port", 0))
	assert_gt(port, 0)
	# S13 A8: the game runs in a run directory of its own, its log there, the build only read; a
	# source run names it (--working-dir), since Godot's --path moves the process to the project.
	var run_dir := String(play.get("run_dir", ""))
	assert_true(run_dir.ends_with("/.opennova/run/runtime/1"), str(play))
	assert_eq(String(play.get("log_file", "")), run_dir.path_join("session.log"), str(play))
	var command_line := String(play.get("command_line", ""))
	assert_true(command_line.contains("--path") and command_line.contains("--working-dir") and
			command_line.contains(run_dir), command_line)
	var game: RefCounted = null
	var deadline := Time.get_ticks_msec() + 30000
	while Time.get_ticks_msec() < deadline:
		game = McpTestClient.new()
		if await game.connect_to(get_tree(), port):
			break
		game.close()
		game = null
		await get_tree().create_timer(0.1).timeout
	if game == null:
		assert_not_null(game, await _output())
	if game != null:
		assert_not_null(await game.initialize(get_tree()))
		var menu_state: Variant = null
		deadline = Time.get_ticks_msec() + 30000
		while Time.get_ticks_msec() < deadline:
			var envelope: Variant = await game.call_tool(get_tree(), "game_menu", {"op": "state"})
			if envelope is Dictionary:
				var content: Dictionary = envelope.get("result", {}).get("structuredContent", {})
				if content.get("visible", false) and content.get("widgets", []).size() > 0:
					menu_state = content
					break
			await get_tree().create_timer(0.2).timeout
		assert_not_null(menu_state, "the menu never became visible")
		if menu_state is Dictionary:
			var texts := {}
			for widget in menu_state.get("widgets", []):
				texts[str(widget.get("name", ""))] = str(widget.get("text", ""))
			assert_eq(texts.get("TITLE", ""), "John Smith's Game")
			assert_eq(texts.get("LATER", ""), "Later")
			assert_not_null(await game.call_tool(get_tree(), "game_menu", {"op": "press", "name": "EXIT"}))
		game.close()
	deadline = Time.get_ticks_msec() + 15000
	var last := {}
	while Time.get_ticks_msec() < deadline:
		last = await _run()
		if String(last.get("state", "")) == "stopped":
			break
		await get_tree().create_timer(0.1).timeout
	assert_eq(String(last.get("state", "")), "stopped", str(last))
	assert_true(bool(last.get("exited_on_its_own", false)), "Exit quits the game")
	var output := await _output()
	assert_true(output.contains("The game exited."), output)
	assert_true(FileAccess.file_exists(run_dir.path_join("session.log")), "the log stays in the run directory")
	assert_false(FileAccess.file_exists(String(built.get("dir", "")).path_join("session.log")),
			"nothing is written into the build")
	assert_eq(_top_level_entries(ProjectSettings.globalize_path("res://")), project_before,
			"a source Play writes nothing in the checkout's Godot project")


# The files and folders at the top of `dir`, sorted, without the tools' own dot-folders (.godot,
# Godot's cache, which a run of the project may write).
func _top_level_entries(dir: String) -> PackedStringArray:
	var names := PackedStringArray()
	for name in DirAccess.get_files_at(dir):
		names.append(name)
	for name in DirAccess.get_directories_at(dir):
		if not name.begins_with("."):
			names.append(name + "/")
	names.sort()
	return names


## The stylesheet document (S9i) through the endpoint: menu_style.mns opens as its lines,
## a variable goes in at a position, DEF_TEXT_FG takes a new colour, a value the game
## would read otherwise is refused, the saved file holds exactly the edits with every
## line ending CR LF, and the build takes it.
func test_stylesheet_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova styles mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var made := await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Styles"})
	assert_true(bool(made.get("ok", false)), str(made))
	assert_true(bool((await _create_missing()).get("ok", false)))
	var opened := await _open("menu_style.mns")
	assert_eq(String(opened.get("kind", "")), "menu_style", str(opened))
	var rows := await _query("document", {"limit": 200})
	var row_list: Array = rows.get("rows", [])
	# Its rows are the variables the game reads (S26: the list), the blank's eleven.
	assert_eq(row_list.size(), 11, str(rows))
	var fg := await _find("%def_text_fg%")
	assert_gt(fg, 0)
	var fg_record := await _record(fg)
	var fg_fields := {}
	for field in fg_record.get("fields", []):
		fg_fields[String(field["id"])] = field
	assert_eq(String(fg_fields.get("value", {}).get("value", "")), "FFFFFFFF", str(fg_record))
	# A new variable right after DEF_TEXT_FG's line.
	var position := -1
	for i in row_list.size():
		if int(row_list[i]["id"]) == fg:
			position = i + 1
	assert_gt(position, 0)
	var brand_red := await _add("variable", 0, position)
	assert_gt(brand_red, 0)
	var named := await _set_field(brand_red, "name", "BRAND_RED")
	assert_true(_done(named), str(named))
	var coloured := await _set_field(brand_red, "value", "FFCC0000")
	assert_true(_done(coloured), str(coloured))
	assert_true(_done(await _set_field(fg, "value", "FF102030")))
	assert_eq(String(await _value(fg, "value")), "FF102030")
	# A lone backslash joins text in the game: refused.
	assert_false(_done(await _set_field(fg, "value", "a\\b")))
	var saved := await _ask("save_all")
	assert_true(_done(saved), str(saved))
	var text := FileAccess.get_file_as_bytes(dir.path_join(String(opened.get("path", "")))).get_string_from_utf8()
	assert_true(text.contains("DEF_TEXT_FG\tFF102030\r\nBRAND_RED\tFFCC0000\r\n"), text)
	assert_false(text.replace("\r\n", "").contains("\n"), "every line ends CR LF")
	# S12 Z2: a set of the value a field holds is no edit: the saved stylesheet stays clean, at its
	# revision.
	var clean := _document_state(await _state(["documents"]), String(opened.get("path", "")))
	assert_false(bool(clean.get("dirty", true)), str(clean))
	assert_true(_done(await _set_field(fg, "value", "FF102030")))
	assert_eq(String(await _value(fg, "value")), "FF102030")
	var after := _document_state(await _state(["documents"]), String(opened.get("path", "")))
	assert_false(bool(after.get("dirty", true)), str(after))
	assert_eq(int(after.get("revision", -1)), int(clean.get("revision", -2)), "no step")
	var built := await _build()
	assert_true(bool(built.get("ok", false)), str(built))


## The Problems rows naming `text`, once the validation an edit left due has made them (it steps a
## frame at a time): the problems query's answer, its `shown` 0 when none came.
func _problems_naming(text: String, want: bool) -> Dictionary:
	var answer := {}
	for _frame in 200:
		answer = await _query("problems", {"text": text})
		if (int(answer.get("shown", 0)) > 0) == want:
			return answer
		await get_tree().process_frame
	return answer


## DI-15: a missing name's first fix is Add it there, an edit_record of the file where the game looks it up,
## passed back through the endpoint as it is: the sound profile an item names, added to SndProf.def and
## selected there, the row gone; Undo of SndProf.def brings it back.
func test_add_it_there_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova add there mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Add there"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	var items := await _open("items.def")
	var item := await _find("100000")
	assert_gt(item, 0, str(items))
	assert_true(_done(await _set_field(item, "sound_profile", "PROF_MCP")))
	var problems := await _problems_naming("PROF_MCP", true)
	assert_eq(int(problems.get("shown", 0)), 1, str(problems))
	if int(problems.get("shown", 0)) != 1:
		return
	var fixes: Array = problems["problems"][0].get("fixes", [])
	assert_gt(fixes.size(), 1, str(fixes))
	var add: Dictionary = fixes[0]
	assert_eq(String(add.get("label", "")), "Add PROF_MCP to SndProf.def", str(add))
	assert_eq(String(add["request"].get("kind", "")), "edit_record")
	assert_true(bool(add["request"].get("open_first", false)))
	var profiles := String(add["request"].get("path", ""))
	var applied := await _call("editor_request", add["request"])
	assert_true(_done(applied), str(applied))
	var documents: Dictionary = (await _state(["documents"])).get("documents", {})
	assert_eq(String(documents.get("active", "")), profiles, str(documents))
	var primary: Dictionary = (await _state(["selection"])).get("selection", {}).get("primary", {})
	var selected := await _query("record", {"path": profiles, "id": int(primary.get("row", 0))})
	assert_eq(String(selected.get("name", "")), "PROF_MCP", str(selected))
	assert_eq(int((await _problems_naming("PROF_MCP", false)).get("shown", -1)), 0)
	assert_true(_done(await _ask("undo", {"path": profiles})))
	assert_eq(int((await _problems_naming("PROF_MCP", true)).get("shown", 0)), 1)


## DI-33: a file the project lacks is made by the engine's own writer from its row: a model and an animation map
## an item names, each Created through the endpoint (the map with the reset clip it names), each row gone; and a
## sound set an item names with no bank to put it in, Created with it (the bank the game searches first, the set
## added there and selected).
func test_create_missing_files_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova create writers mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Writers"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	await _open("items.def")
	var added := await _edit([{"op": "add", "kind": "item", "field": "display_name", "value": "Door", "as": "door"},
			{"op": "set", "id": "door", "field": "graphic", "value": "di33box"},
			{"op": "set", "id": "door", "field": "anim_def", "value": "di33map"},
			{"op": "set", "id": "door", "field": "door_open_sound", "value": "DI33_OPEN"}])
	assert_true(_done(added), str(added))
	for pair: Array in [["di33box", "Create di33box.3di"], ["di33map", "Create di33map.adm"],
			["DI33_OPEN", "Create gamelocl.lwf with DI33_OPEN"]]:
		var problems := await _problems_naming(String(pair[0]), true)
		assert_eq(int(problems.get("shown", 0)), 1, str(problems))
		if int(problems.get("shown", 0)) != 1:
			return
		var fixes: Array = problems["problems"][0].get("fixes", [])
		assert_gt(fixes.size(), 0, str(fixes))
		if fixes.is_empty():
			return
		var create: Dictionary = fixes[0]
		assert_eq(String(create.get("label", "")), String(pair[1]), str(create))
		assert_eq(String(create["request"].get("kind", "")), "create_file")
		var applied := await _call("editor_request", create["request"])
		assert_true(_done(applied), str(applied))
		# The reference resolves (a new set with no layer has a finding of its own, which names it too).
		var codes: Array[String] = []
		for row: Variant in (await _query("problems", {"text": String(pair[0])})).get("problems", []):
			codes.append(String((row as Dictionary).get("code", "")))
		assert_does_not_have(codes, "reference.missing", String(pair[0]))
	var names: Array[String] = []
	for file: Variant in (await _query("files", {"limit": 200})).get("files", []):
		names.append(String((file as Dictionary).get("name", "")))
	assert_has(names, "di33map_rst.bad", str(names))
	var bank := String((await _state(["documents"])).get("documents", {}).get("active", ""))
	assert_true(bank.ends_with("gamelocl.lwf"), bank)
	var primary: Dictionary = (await _state(["selection"])).get("selection", {}).get("primary", {})
	var selected := await _query("record", {"path": bank, "id": int(primary.get("row", 0))})
	assert_eq(String(selected.get("name", "")), "DI33_OPEN", str(selected))


## The asset graph through the endpoint (S7), one request of each family: a blank project's references all
## resolve, a page of the symbols, a reference field's choices, completion and Go to places, a find in the
## menu and in the project, who uses the stylesheet; a texture the menu names found from both ends, renamed (the
## unsaved prompt first, its save writing the menu) with the menu rewritten, moved to a folder with nothing
## rewritten, deleted to the trash and brought back; a style variable renamed everywhere, planned then
## committed. The rules each family's request reaches are pinned below the wire: the lists' pages and
## refusals (editor_query_test's test_paging and test_refusals), the choices and places (asset_graph_test,
## mission_labels_test), the finds (document_search_test, jumps_test), the rename, its refusals and its
## prompt (editor_session_test's test_outcomes_and_refusals, asset_graph_test's test_rename,
## unsaved_guard_test), a move and its way back (file_placement_test's test_move), the trash, a duplicate and
## the folders (file_chores_test's test_delete_named and test_folders), a symbol renamed (symbol_rename_test).
func test_graph_references_and_rename() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova graph mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Graph"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	var references := await _query("references", {"path": "main.mnu"})
	assert_gt(int(references.get("count", 0)), 0, str(references))
	for edge in references.get("edges", []):
		assert_eq(String(edge.get("status", "")), "present", str(edge))
	assert_eq(int((await _query("missing")).get("count", -1)), 0)
	var all_symbols := await _query("symbols")
	var symbol_page := await _query("symbols", {"offset": 1, "limit": 2})
	assert_eq(int(symbol_page.get("count", -1)), int(all_symbols.get("count", 0)))
	assert_eq(symbol_page.get("symbols", []).size(), 2, str(symbol_page))
	# A texture the menu names: a 4x4 TGA (the menu opens on its first screen, which the preview draws,
	# decoding it; Godot's reader wants more bytes than a 1x1 file has).
	var root: String = (await _state(["project"]))["project"]["root"]
	TestFs.write_bytes(self, root.path_join("logo.tga"), TestFs.tga_bytes(Vector2i(4, 4)))
	assert_true(bool((await _call("editor_request", {"kind": "rescan"})).get("ok", false)))
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	var exit_id := await _find("EXIT")
	assert_gt(exit_id, 0)
	# S12 Z2, DI-09: MAIN's font names a style variable: the picker offers the project's fonts and the
	# stylesheet's variables, completion begins with what is typed, Go to leads to the variable where the game
	# reads it, then to the .fnt its value names.
	var main_id := await _find("MAIN", "MAIN.MNU/STARTUP")
	assert_gt(main_id, 0)
	var choices := await _query("reference_choices", {"id": main_id, "field": "font.name"})
	var offered := {}
	for choice in choices.get("choices", []):
		offered[String(choice.get("kind", ""))] = true
	assert_true(offered.has("font") and offered.has("style_var"), str(choices))
	var completed := await _query("complete", {"id": main_id, "field": "font.name", "prefix": "%def_font"})
	var completions: Array = completed.get("choices", [])
	assert_true(not completions.is_empty() and String(completions[0].get("name", "")).to_upper().begins_with("%DEF_FONT"),
			str(completed))
	var places: Array = (await _query("reference_targets", {"id": main_id, "field": "font.name"})).get("targets", [])
	assert_eq(places.size(), 2, str(places))
	if places.size() == 2:
		assert_true(String(places[0].get("file", "")).ends_with("menu_style.mns"), str(places[0]))
		assert_true(String(places[1].get("file", "")).to_lower().ends_with(".fnt"), str(places[1]))
	# S12 D8: a find in the open menu, and in the project; who uses the stylesheet.
	var exit_hit := false
	for hit in (await _query("document_search", {"text": "exit"})).get("hits", []):
		exit_hit = exit_hit or (int(hit.get("id", 0)) == exit_id and String(hit.get("field", "")) == "name")
	assert_true(exit_hit)
	var found_exit := false
	for hit in (await _query("project_search", {"text": "exi"})).get("hits", []):
		found_exit = found_exit or (String(hit.get("kind", "")) == "menu_window" and String(hit.get("name", "")) == "EXIT"
				and hit.has("usages"))
	assert_true(found_exit)
	var style_edges: Array = (await _query("usages", {"path": "menu_style.mns"})).get("edges", [])
	assert_true(not style_edges.is_empty() and not String(style_edges[0].get("locator", "")).is_empty(), str(style_edges))
	# EXIT's first APPEARANCE row made an image (the rows are records of their own): the texture's user.
	var exit_record := await _record(exit_id)
	var row_id := 0
	for collection in exit_record.get("collections", []):
		if String(collection.get("kind_name", "")) == "appearance":
			row_id = int(collection["records"][0]["id"])
	assert_gt(row_id, 0, str(exit_record))
	assert_true(_done(await _set_field(row_id, "type", "IMAGE")))
	assert_true(_done(await _set_field(row_id, "value", "logo.tga")))
	var users := await _query("referrers", {"path": "logo.tga"})
	assert_eq(int(users.get("count", 0)), 1, str(users))
	assert_eq(String(users["edges"][0].get("record", "")), "STARTUP/MAIN/EXIT/Appearance 1")
	# The menu that names it has unsaved edits: the rename waits on the unsaved prompt; its save writes the
	# menu and renames, the menu naming the new name.
	var waiting := await _call("editor_request", {"kind": "rename_asset", "path": "logo.tga", "new_name": "logo2.tga"})
	assert_true(bool(waiting.get("outcome", {}).get("unsaved_prompt", false)) and not _done(waiting), str(waiting))
	var renamed := await _call("editor_request", {"kind": "resolve_unsaved", "choice": "save"})
	assert_true(bool(renamed.get("outcome", {}).get("done", false)), str(renamed))
	assert_true(FileAccess.file_exists(root.path_join("logo2.tga")) and not FileAccess.file_exists(root.path_join("logo.tga")))
	var reloaded_exit := await _record(await _find("EXIT"))
	var reloaded_row := 0
	for collection in reloaded_exit.get("collections", []):
		if String(collection.get("kind_name", "")) == "appearance":
			reloaded_row = int(collection["records"][0]["id"])
	assert_eq(String(await _value(reloaded_row, "value")), "logo2.tga")
	# DI-03: a move to another folder rewrites nothing (the game finds a file by its name alone).
	assert_true(_done(await _call("editor_request", {"kind": "move_asset", "path": "logo2.tga", "folder": "art"})))
	assert_true(FileAccess.file_exists(root.path_join("art/logo2.tga")))
	assert_eq(String(await _value(reloaded_row, "value")), "logo2.tga")
	assert_eq(int((await _query("missing")).get("count", -1)), 0)
	# DI-25: deleted to the project's trash when asked to leave its use, and the file history brings it back.
	var deleted := await _call("editor_request", {"kind": "delete_asset", "path": "art/logo2.tga", "force": true})
	assert_true(_done(deleted), str(deleted))
	assert_gt(int((await _query("missing")).get("count", -1)), 0)
	assert_true(_done(await _call("editor_request", {"kind": "undo_file"})))
	assert_true(FileAccess.file_exists(root.path_join("art/logo2.tga")))
	# S12 D9: a style variable the blank menu names renamed everywhere: planned (the dialogs section's
	# rename_preview), then committed; every use still resolves, under the new name.
	var variable: Dictionary = {}
	for symbol in (await _query("symbols", {"kind": "style_var"})).get("symbols", []):
		if not bool(symbol.get("inert", false)) and String(symbol.get("name", "")) == "DEF_FONTNAME_LG":
			variable = symbol
	assert_false(variable.is_empty(), "DEF_FONTNAME_LG is defined where the game reads it")
	var rename_args := {"path": String(variable.get("file", "")), "locator": String(variable.get("locator", "")),
			"field": String(variable.get("field", "")), "new_name": "DEF_FONTNAME_BIG"}
	assert_true(_done(await _call("editor_request", {"kind": "preview_rename"}.merged(rename_args))))
	var sites: Array = (await _state(["dialogs"])).get("dialogs", {}).get("rename_preview", {}).get("sites", [])
	assert_true(not sites.is_empty() and String(sites[0].get("after", "")) == "DEF_FONTNAME_BIG", str(sites))
	var committed := await _call("editor_request", {"kind": "rename_symbol"}.merged(rename_args))
	assert_true(bool(committed.get("outcome", {}).get("done", false)), str(committed))
	var big := false
	for symbol in (await _query("symbols", {"kind": "style_var"})).get("symbols", []):
		big = big or String(symbol.get("name", "")) == "DEF_FONTNAME_BIG"
	assert_true(big, "the variable renamed")
	assert_eq(int((await _query("missing")).get("count", -1)), 0)


## A request's outcome through the endpoint (S9b): `ok` says the request read, the outcome says whether it
## happened. A badly named new document is refused with its finding and writes nothing; a new menu of its own
## gets one screen named after the file; closing it with unsaved changes waits on the prompt, which the
## outcome says. create_file's other refusals (a kind its extension does not take, a name past the archive's
## 16 bytes) are editor_session_test's test_outcomes_and_refusals.
func test_request_outcomes() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova outcome mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Outcomes"})).get("ok", false)))
	var filled := await _create_missing()
	assert_true(bool(filled.get("outcome", {}).get("done", false)), str(filled))
	var bad := await _call("editor_request", {"kind": "create_file", "path": "../x.mnu"})
	assert_true(bool(bad.get("ok", false)), "the request read")
	assert_true(not _done(bad) and _found(bad, "document.name"), str(bad))
	assert_false(FileAccess.file_exists(dir.path_join("x.mnu")))
	var extra := await _call("editor_request", {"kind": "create_file", "path": "extra.mnu", "file_kind": "menu"})
	assert_true(_done(extra), str(extra))
	var rows := await _query("document", {"path": "extra.mnu"})
	assert_eq(String(rows.get("kind", "")), "menu")
	assert_eq(int(rows.get("row_count", 0)), 1, str(rows))
	assert_eq(String(rows.get("rows", [{}])[0].get("name", "")), "EXTRA")
	var main := await _find("MAIN")
	assert_gt(main, 0)
	assert_true(_done(await _set_field(main, "position.left", 8)))
	var closing := await _call("editor_request", {"kind": "close_document", "path": "extra.mnu"})
	assert_true(bool(closing.get("outcome", {}).get("unsaved_prompt", false)) and not _done(closing), str(closing))
	var discarded := await _call("editor_request", {"kind": "resolve_unsaved", "choice": "discard"})
	assert_true(bool(discarded.get("outcome", {}).get("done", false)), str(discarded))
	for document in (await _query("documents")).get("documents", []):
		assert_ne(String(document.get("path", "")), "menus/extra.mnu", "discarded and closed")


## Every scanned file is reachable past the result's list cap (PAGE_MAX): the files query's first page holds
## PAGE_MAX of them and counts them all, the last page the last one with no page after it, and the project
## section counts them. Every page read in turn, each entry once, is editor_query_test's test_paging.
func test_file_list_pages_past_the_cap() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova files mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Files"})).get("ok", false)))
	assert_eq(DirAccess.make_dir_recursive_absolute(dir.path_join("notes")), OK)
	for i in EditorMcpCatalog.PAGE_MAX + 10:
		TestFs.write_text(self, dir.path_join("notes/n%03d.txt" % i), "note")
	assert_true(bool((await _call("editor_request", {"kind": "rescan"})).get("ok", false)))
	var first := await _query("files", {"limit": EditorMcpCatalog.PAGE_MAX})
	var total := int(first.get("count", 0))
	assert_gt(total, EditorMcpCatalog.PAGE_MAX, str(first.keys()))
	assert_eq(first.get("files", []).size(), EditorMcpCatalog.PAGE_MAX)
	var last := await _query("files", {"offset": total - 1, "limit": 5})
	assert_eq(int(last.get("count", 0)), total)
	assert_eq(last.get("files", []).size(), 1, "the last page of the files")
	assert_eq(last.get("next_offset"), null, "no page after the last")
	assert_eq(int((await _state(["project"])).get("project", {}).get("file_count", 0)), total)


## S13 V7: a model's viewport through editor_viewport. Its camera set (a set_viewport, the state after
## it its answer's viewport); the preview clock sought with op seek, which names no document and no
## kind (path and kind refused) and sets the clock whatever is active (a stylesheet included, its
## answer then naming no viewport); the viewport of a kind named, and a kind that does not show the
## document refused; a hit naming the viewport it read.
func test_model_viewport_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova viewport mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Models"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	var root: String = (await _state(["project"]))["project"]["root"]
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/threedi/synth/armory.3di"))
	assert_gt(bytes.size(), 0, "the synthetic armory model")
	assert_eq(DirAccess.make_dir_recursive_absolute(root.path_join("models")), OK)
	var file := FileAccess.open(root.path_join("models/armory.3di"), FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()
	assert_true(_done(await _call("editor_request", {"kind": "rescan", "wait": true})))
	assert_true(_done(await _call("editor_request", {"kind": "open_document", "path": "models/armory.3di"})))
	var state := await _call("editor_viewport", {"op": "state", "limit": 1})
	assert_eq(String(state.get("kind", "")), "model", str(state))
	assert_eq(String(state.get("units", "")), "pixels", str(state))
	# The camera: a set_viewport of it, the state after it its answer's viewport.
	var turned := await _call("editor_viewport", {"op": "camera", "camera": {"yaw": 1.25, "distance": 12}})
	assert_true(_done(turned), str(turned))
	assert_almost_eq(float(turned.get("viewport", {}).get("camera", {}).get("yaw", 0.0)), 1.25, 0.0001, str(turned))
	assert_almost_eq(float(turned.get("viewport", {}).get("camera", {}).get("distance", 0.0)), 12.0, 0.0001)
	# seek: the clock alone, no path and no kind.
	var sought := await _call("editor_viewport", {"op": "seek", "clock": {"playing": false, "time_ms": 250}})
	assert_true(_done(sought), str(sought))
	var clock: Dictionary = sought.get("viewport", {}).get("clock", {})
	assert_false(bool(clock.get("playing", true)), str(sought))
	assert_eq(int(clock.get("time_ms", 0)), 250, str(sought))
	assert_true(String((await _call("editor_viewport", {"op": "seek", "path": "models/armory.3di",
			"clock": {"time_ms": 1}})).get("_error", "")).contains("takes no \"path\""))
	assert_true(String((await _call("editor_viewport", {"op": "seek", "kind": "model",
			"clock": {"time_ms": 1}})).get("_error", "")).contains("takes no \"kind\""))
	# The viewport of a kind named; a kind that does not show the document refused.
	assert_eq(String((await _call("editor_viewport", {"op": "state", "kind": "model", "limit": 1})).get("kind", "")), "model")
	assert_true(String((await _call("editor_viewport", {"op": "state", "kind": "menu"})).get("_error", "")).contains(
			"does not show in a menu viewport"))
	# A hit names the viewport it read beside the marker it found.
	var hits := 0
	for item: Variant in (await _call("editor_viewport", {"op": "items", "limit": 200})).get("items", []):
		var marker: Dictionary = item
		if String(marker.get("kind", "")) != "user_point" or not (marker.get("screen") is Array):
			continue
		var hit := await _call("editor_viewport", {"op": "hit", "x": float(marker["screen"][0]), "y": float(marker["screen"][1])})
		assert_eq(String(hit.get("viewport", "")), "model", str(hit))
		assert_eq(String(hit.get("path", "")), "models/armory.3di", str(hit))
		assert_eq(int(hit.get("id", 0)), int(marker.get("id", -1)), str(hit))
		hits += 1
		break
	assert_eq(hits, 1, "a user point on the picture")
	# A stylesheet active: seek is done all the same (it names no document), its answer naming no
	# viewport; the model's reads the clock it set.
	assert_true(_done(await _call("editor_request", {"kind": "open_document", "path": "menu_style.mns"})))
	var still := await _call("editor_viewport", {"op": "seek", "clock": {"time_ms": 500}})
	assert_true(_done(still), str(still))
	assert_false(still.has("viewport"), str(still))
	var read := await _call("editor_viewport", {"op": "state", "path": "models/armory.3di", "limit": 1})
	assert_eq(int(read.get("clock", {}).get("time_ms", 0)), 500, str(read))


## The image importer through the endpoint (S8): a PNG imported into the project becomes a
## 32-bit TGA under the cache (S18: the default, its alpha kept) with a committed sidecar beside the
## source, the state lists the import, the graph resolves the output as a texture, and the source
## itself is never a build member. A PNG saved into the project with no record is a texture the game
## loads as it is (S9p2a). S18: the import_options query answers its options; set_import_options
## makes it a PCX, the TGA gone.
func test_png_import_through_the_endpoint() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova import mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Imports"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	var root: String = (await _state(["project"]))["project"]["root"]
	var image := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	for y in 16:
		for x in 16:
			image.set_pixel(x, y, Color(float(x) / 15.0, float(y) / 15.0, 0.5, 1.0))
	assert_eq(DirAccess.make_dir_recursive_absolute(root.path_join("art")), OK)
	assert_eq(image.save_png(root.path_join("art/plain.png")), OK)
	assert_true(bool((await _call("editor_request", {"kind": "rescan"})).get("ok", false)))
	var plain := await _state(["import"])
	assert_eq(plain.get("import", {}).get("imported", []).size(), 0, "a PNG with no record is not imported")
	assert_false(FileAccess.file_exists(root.path_join("art/plain.png.import")), "the scan writes no record")
	var outside := dir + " src"
	_dirs.append(outside)
	assert_eq(DirAccess.make_dir_recursive_absolute(outside), OK)
	assert_eq(image.save_png(outside.path_join("logo.png")), OK)
	var importing := await _call("editor_request", {"kind": "import_files", "imports": [{"path": outside.path_join("logo.png")}]})
	assert_true(bool(importing.get("outcome", {}).get("done", false)), str(importing))
	var state := await _state(["import"])
	var imported: Array = state.get("import", {}).get("imported", [])
	assert_eq(imported.size(), 1, str(state.get("import", {})))
	if imported.size() == 1:
		# An author's PNG lands where the project keeps the textures its import makes (DI-03): one in
		# textures/ (the pointer Create missing made) and one in art/ (plain.png), so the kind's own folder.
		assert_eq(String(imported[0].get("source", "")), "textures/logo.png")
		assert_true(bool(imported[0].get("ok", false)))
		assert_eq(imported[0].get("outputs", []).size(), 1)
		assert_true(FileAccess.file_exists(root.path_join(String(imported[0]["outputs"][0]))))
		assert_eq(imported[0].get("inputs", null), [], "the image importer reads its source alone")
	assert_true(FileAccess.file_exists(root.path_join("textures/logo.png.import")), "importing writes the record beside the source")
	var problems := await _query("problems", {"severities": ["error"]})
	assert_eq(int(problems.get("shown", -1)), 0, str(problems))
	var symbols := await _query("referrers", {"path": "logo.tga"})
	assert_eq(int(symbols.get("count", -1)), 0)
	var options := await _query("import_options", {"path": "logo.tga"})
	assert_eq(String(options.get("source", "")), "textures/logo.png", str(options))
	assert_eq(String(options.get("effective", {}).get("format", "")), "tga", str(options))
	var made_pcx := await _call("editor_request", {"kind": "set_import_options", "path": "textures/logo.png", "values": {"format": "pcx"}})
	assert_true(_done(made_pcx), str(made_pcx))
	var renamed := await _query("files", {"limit": 200})
	var outputs: Array[String] = []
	for file in renamed.get("files", []):
		outputs.append(String(file["name"]))
	assert_has(outputs, "logo.pcx", "the import makes the PCX")
	assert_does_not_have(outputs, "logo.tga", "the TGA it made before is gone")
	var listed := await _query("files", {"limit": 200})
	var names: Array[String] = []
	var kinds := {}
	for file in listed.get("files", []):
		kinds[String(file["name"])] = String(file.get("kind", ""))
		if bool(file.get("editable", false)):
			names.append(String(file["name"]))
	assert_does_not_have(names, "logo.png", "a source is not an editable file")
	# S13 A8: an import source is its own kind while its record is there; a PNG with none, a texture.
	assert_eq(kinds.get("logo.png", ""), "import_source", str(kinds))
	assert_eq(kinds.get("plain.png", ""), "texture", str(kinds))
	var again := await _call("editor_request", {"kind": "reimport", "force": true})
	assert_true(bool(again.get("ok", false)), str(again))
	assert_true(String(again.get("status", "")).contains("1 source"), str(again))


## S11g: an import with the files it needs, through the endpoint. preview_import with its flag
## plans a menu, the font and the texture beside it and a texture found nowhere: the
## import_preview query's rows (the menu chosen, each dependency with what needs it and where it
## was found, copied as the game's own), the not_found row with what needs it. import_files with
## the rows' sources passed back as they come writes them, and the menu's references resolve but
## the one not found. set_import_dependencies writes the editor's setting, which the import
## section says. A plan longer than a page is read page by page, no row lost.
func test_import_with_dependencies_through_the_endpoint() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova import deps mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Dependencies"})).get("ok", false)))
	var art := dir + " art"
	_dirs.append(art)
	assert_eq(DirAccess.make_dir_recursive_absolute(art), OK)
	var position := "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM></POSITION>\r\n"
	var menu := "<SCREEN>\r\n<NAME>A</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"GO\">\r\n" + position \
			+ "<FONT><NAME>arial99</NAME></FONT>\r\n<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">logo.tga</APPEARANCE>\r\n" \
			+ "</WINDOW>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"KEEP\">\r\n" + position \
			+ "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">gone.tga</APPEARANCE>\r\n</WINDOW>\r\n</SCREEN>\r\n"
	for file: Array in [["a.mnu", menu], ["arial99.fnt", "fnt"], ["LOGO.TGA", "tga"]]:
		var out := FileAccess.open(art.path_join(String(file[0])), FileAccess.WRITE)
		assert_not_null(out)
		out.store_string(String(file[1]))
		out.close()
	var preview := await _call("editor_request", {"kind": "preview_import", "paths": [art.path_join("a.mnu")], "with_dependencies": true})
	assert_true(bool(preview.get("outcome", {}).get("done", false)), str(preview))
	var summary: Dictionary = (await _state(["import"])).get("import", {})
	assert_true(bool(summary.get("open", false)) and bool(summary.get("with_dependencies", false)), str(summary))
	assert_eq(int(summary.get("row_count", 0)), 3, str(summary))
	var planned := await _query("import_preview")
	assert_true(bool(planned.get("open", false)) and bool(planned.get("with_dependencies", false)), str(planned))
	var rows: Array = planned.get("rows", [])
	assert_eq(rows.size(), 3, str(rows))
	var names: Array[String] = []
	for row: Dictionary in rows:
		names.append(String(row.get("name", "")))
		if String(row.get("name", "")) == "a.mnu":
			assert_eq(String(row.get("state", "")), "selected")
			assert_false(bool(row.get("source", {}).get("native", false)), "a file picked stays the author's")
		else:
			assert_eq(String(row.get("state", "")), "found", str(row))
			assert_eq(String(row.get("needed_by", {}).get("file", "")), "a.mnu", str(row))
			assert_true(String(row.get("found_in", "")).begins_with("the folder "), str(row))
			assert_true(bool(row.get("source", {}).get("native", false)), "a dependency is copied as the game's own")
	assert_eq(names[0] if not names.is_empty() else "", "a.mnu", "the chosen file first")
	assert_has(names, "arial99.fnt")
	assert_has(names, "LOGO.TGA")
	var missing: Array = planned.get("not_found", [])
	assert_eq(missing.size(), 1, str(missing))
	if missing.size() == 1:
		assert_eq(String(missing[0].get("name", "")), "gone.tga")
		assert_eq(String(missing[0].get("needed_by", {}).get("record", "")), "A/KEEP/Appearance 1")
	var sources := []
	for row: Dictionary in rows:
		if bool(row.get("selected", false)):
			sources.append(row["source"])
	var importing := await _call("editor_request", {"kind": "import_files", "imports": sources})
	assert_true(bool(importing.get("outcome", {}).get("done", false)), str(importing))
	assert_false(bool((await _state(["import"])).get("import", {}).get("open", true)), "the preview closed")
	var statuses := {}
	for edge: Dictionary in (await _query("references", {"path": "a.mnu"})).get("edges", []):
		statuses[String(edge.get("value", ""))] = String(edge.get("status", ""))
	assert_eq(String(statuses.get("arial99", "")), "present", str(statuses))
	assert_eq(String(statuses.get("logo.tga", "")), "present", str(statuses))
	assert_eq(String(statuses.get("gone.tga", "")), "missing", str(statuses))
	var setting := await _call("editor_request", {"kind": "set_import_dependencies", "with_dependencies": false})
	assert_true(bool(setting.get("outcome", {}).get("done", false)), str(setting))
	assert_false(bool((await _state(["import"])).get("import", {}).get("import_dependencies", true)))
	# A plan longer than a page (the transport caps a list at 200): 201 files chosen, the first
	# page holds 200 rows and the count, the next page reaches the last, whose source passes
	# back to import_files as it is.
	var many: Array = []
	for i in 201:
		var path := art.path_join("n%03d.txt" % i)
		var out := FileAccess.open(path, FileAccess.WRITE)
		out.store_string("n")
		out.close()
		many.append(path)
	assert_true(bool((await _call("editor_request", {"kind": "preview_import", "paths": many})).get("outcome", {}).get("done", false)))
	var first := await _query("import_preview", {"limit": 200})
	assert_eq(int(first.get("count", 0)), 201)
	assert_eq(first.get("rows", []).size(), 200, "a page of 200, no row cut by the transport")
	assert_eq(int(first.get("next_offset", -1)), 200)
	var last := await _query("import_preview", {"offset": 200, "limit": 200})
	var tail: Array = last.get("rows", [])
	assert_eq(tail.size(), 1, str(last))
	if tail.size() == 1:
		assert_eq(String(tail[0].get("name", "")), "n200.txt")
		var one := await _call("editor_request", {"kind": "import_files", "imports": [tail[0]["source"]]})
		assert_true(bool(one.get("outcome", {}).get("done", false)), str(one))
		assert_true(FileAccess.file_exists((await _state(["project"]))["project"]["root"].path_join("n200.txt")))


## S9k2 through the endpoint: TITLE and EXIT copied and pasted into a second screen's MAIN (the copies
## selected, under that screen), then arranged in the viewport (EXIT's left edge to TITLE's: one request, one
## undo step, no gesture). The rest of the clipboard (nothing selected, into another file, a duplicate) is
## mnu_document_test's copy_and_paste; the arrange commands and their refusals are menu_arrange_test's.
func test_clipboard_across_screens_and_arrange() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova clipboard mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Clip"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	var title := await _find("TITLE")
	var exit := await _find("EXIT")
	assert_gt(title, 0)
	assert_gt(exit, 0)
	var second := await _add("screen")
	assert_gt(second, 0)
	var second_main := 0
	for row: Variant in (await _query("document")).get("rows", []):
		if row is Dictionary and int((row as Dictionary).get("id", 0)) == second:
			second_main = int((row as Dictionary)["collections"][0]["records"][0]["id"])
	assert_gt(second_main, 0)
	assert_true(bool((await _select(title)).get("ok", false)))
	assert_true(bool((await _select(exit, "add")).get("ok", false)))
	assert_true(_done(await _ask("copy")))
	var pasted := await _ask("paste", {"paste_at": {"parent": second_main}})
	assert_true(_done(pasted), str(pasted))
	var selected: Array = (await _state(["selection"])).get("selection", {}).get("records", [])
	assert_eq(selected.size(), 2, str(selected))
	if selected.size() == 2:
		var copy_record := await _record(int((selected[0] as Dictionary).get("child", 0)))
		assert_true(String(copy_record.get("path", "")).ends_with("/MAIN/TITLE"), str(copy_record))
		assert_eq(int((copy_record.get("owner", {}) as Dictionary).get("row", 0)), second, str(copy_record))
	# The originals selected again, the viewport shows their screen.
	assert_true(bool((await _select(title)).get("ok", false)))
	assert_true(bool((await _select(exit, "add")).get("ok", false)))
	var arranged := await _call("editor_viewport", {"op": "command", "command": {"name": "align_left", "ids": [title, exit]}})
	assert_true(_done(arranged), str(arranged))
	assert_false(arranged.get("outcome", {}).has("gesture"), "a command carries no gesture")
	var rects: Dictionary = arranged.get("viewport", {})
	assert_eq(int(_rect_of(rects, "EXIT")[0]), int(_rect_of(rects, "TITLE")[0]), str(rects))
	assert_true(_done(await _ask("undo")))
	assert_eq(int(_rect_of(await _call("editor_viewport", {"op": "items", "limit": 200}), "EXIT")[0]), 340)


## DI-35: a menu's Try mode through editor_viewport: op try turns it on from the screen shown, op click is the
## game's click (a new project's EXIT: the quit said, not done, the screen staying), answering with the
## viewport, its body's try saying where the menu is and what the game would have done; off again. Keys,
## Reset and the refusals are menu_try_test's.
func test_try_mode_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova try mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Try"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	var rect := _rect_of(await _call("editor_viewport", {"op": "items", "limit": 200}), "EXIT")
	assert_eq(rect.size(), 4)
	if rect.size() != 4:
		return
	var at := [(float(rect[0]) + float(rect[2])) / 2.0, (float(rect[1]) + float(rect[3])) / 2.0]
	var on := await _call("editor_viewport", {"op": "try", "try": {"on": true}})
	assert_true(_done(on), str(on))
	assert_eq(String(on.get("viewport", {}).get("body", {}).get("try", {}).get("screen", "")), "STARTUP", str(on))
	var clicked := await _call("editor_viewport", {"op": "click", "click": {"at": at}})
	assert_true(_done(clicked), str(clicked))
	var tried: Dictionary = clicked.get("viewport", {}).get("body", {}).get("try", {})
	var last: Dictionary = tried.get("last", {}) if tried.get("last") is Dictionary else {}
	assert_eq(String(last.get("kind", "")), "exit", str(tried))
	assert_eq(String(last.get("control", "")), "EXIT", str(last))
	assert_eq(String(tried.get("screen", "")), "STARTUP", "the quit said, not done")
	var off := await _call("editor_viewport", {"op": "try", "try": {"on": false}})
	assert_true(_done(off), str(off))
	assert_false(bool(off.get("viewport", {}).get("body", {}).get("try", {}).get("on", true)))


## A window of a menu_tree answer by name (an empty Dictionary when it is not there).
func _tree_window(tree: Dictionary, name: String) -> Dictionary:
	for screen: Variant in tree.get("screens", []):
		if not (screen is Dictionary):
			continue
		for window: Variant in (screen as Dictionary).get("windows", []):
			if window is Dictionary and String((window as Dictionary).get("name", "")) == name:
				return window as Dictionary
	return {}


## S9m through the endpoint, as edit_record's batch form (S13 A5): a button with two ACTIONs added and filled in
## by label in one batch (the ids answered in the outcome's made and added), and a record's fields set in the
## order the client wrote them (a body's draw kind, then its flag cleared, leaves no draw kind: sorted keys
## would clear the flag first). The menu reads (menu_tree, menu_findings), their pages and refusals, the
## batch's undo step, replace_list and its refusals are editor_query_test's test_menu_reads_and_batches.
func test_menu_tools_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova menu tools mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Tools"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	var main := int(_tree_window(await _query("menu_tree"), "MAIN").get("id", 0))
	assert_gt(main, 0)
	var batch := await _edit([
		{"op": "add", "kind": "window", "parent": main, "as": "hello"},
		{"op": "set", "id": "hello", "field": "name", "value": "HELLO"},
		{"op": "set", "id": "hello", "field": "type", "value": "BUTTON"},
		{"op": "add", "kind": "action", "parent": "hello", "as": "back"},
		{"op": "add", "kind": "action", "parent": "hello", "as": "show"},
		{"op": "set", "id": "show", "field": "type", "value": "WINDOW"},
		{"op": "set", "id": "show", "field": "target", "value": "TITLE"},
	], "main.mnu")
	assert_true(bool(batch.get("outcome", {}).get("done", false)), str(batch))
	var made: Dictionary = batch.get("outcome", {}).get("made", {})
	assert_eq(made.size(), 3, str(batch))
	assert_eq((batch.get("outcome", {}).get("added", []) as Array).size(), 3, str(batch))
	var hello := int(made.get("hello", 0))
	assert_eq(await _find("HELLO"), hello)
	assert_eq(String(await _value(int(made.get("show", 0)), "target")), "TITLE")
	# HELLO a TABLE for the leg: a button reads no COLUMN, so its list takes no record.
	assert_true(_done(await _edit([{"op": "set", "id": hello, "field": "type", "value": "table"}], "main.mnu")))
	var drawn := await _edit([{"op": "replace_list", "id": hello, "list": "column.body",
			"records": [{"display": "CUSTOM_DRAW", "custom_draw": 0}]}], "main.mnu")
	assert_true(bool(drawn.get("outcome", {}).get("done", false)), str(drawn))
	var drawn_added: Array = drawn.get("outcome", {}).get("added", [])
	assert_eq(drawn_added.size(), 1, str(drawn))
	if drawn_added.size() == 1:
		assert_eq(String(await _value(int(drawn_added[0]), "display")), "", "the flag cleared after the draw kind")
		assert_eq(int(await _value(int(drawn_added[0]), "custom_draw")), 0)
