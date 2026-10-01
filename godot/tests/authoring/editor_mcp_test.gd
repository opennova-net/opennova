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
## operation, read mid-way through editor_state while it steps frame by frame, and
## editor_build joins it and waits for it to land. S13 A4: the request table on the wire (the
## kind enum, the fields each kind takes, the game install's words). S13 A5: one read seam and
## one write seam: editor_state answers the view by section, editor_query every other read
## (the files, a document and its records, the problems, the asset graph, a menu's tree,
## findings and render, the import plan, the output, the operation, the catalog), each list a
## page; editor_request raises every change, a record batch in the batch form (records by
## identity or by the label an earlier add gave, kinds by token, a list replaced), its outcome
## naming what it added and what each label made; the tool list and the request enum come from
## `editor_query catalog`, and editor_document, editor_graph, editor_problems and editor_menu
## are gone.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const McpTestClient := preload("res://tests/mcp/mcp_test_client.gd")
const CATALOG: Array[String] = [
	"editor_state", "editor_request", "editor_query", "editor_build", "editor_play", "editor_menu_preview",
	"editor_model_preview", "editor_screenshot", "editor_logs",
]

var _dirs: Array[String] = []
var _app: Node = null
var _service: EditorMcpService = null
var _client: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	var settings_dir := OS.get_cache_dir().path_join("opennova editor mcp %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)
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
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


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
## total, cancellable, holding the files), editor_build joins it and returns once it lands, and
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
	var built := await _call("editor_build")
	assert_true(bool(built.get("ok", false)), str(built))
	assert_eq(int(built.get("operation", {}).get("id", 0)), id, "editor_build joined the build that ran")
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


## A widget's absolute rect in an editor_menu_preview rects page (all zero when it is not there).
func _rect_of(rects: Dictionary, name: String) -> Array:
	for widget: Variant in rects.get("widgets", []):
		if widget is Dictionary and String((widget as Dictionary).get("name", "")) == name:
			return (widget as Dictionary).get("rect", [0, 0, 0, 0])
	return [0, 0, 0, 0]


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
	assert_true((await _call("editor_request", {"kind": "build", "flagg": true})).get("_error", "").contains("flagg"),
			"an unknown member is refused, not ignored")
	assert_true((await _query("document")).has("_error"), "no document is open")
	assert_true((await _call("editor_build")).get("_error", "").contains("No project"))
	assert_true((await _call("editor_play", {"op": "start"})).get("_error", "").contains("No project"))
	assert_true((await _call("editor_screenshot")).get("_error", "").contains("headless"))
	var preview := await _call("editor_menu_preview", {"op": "state"})
	assert_eq(String(preview.get("status", "")), "no_project", str(preview))
	assert_eq(int(preview.get("widget_count", -1)), 0)
	assert_true((await _query("menu_tree")).get("_error", "").contains("no document is active"), "no project, no menu")
	assert_true((await _query("nope")).get("_error", "").contains("Unknown query"), "an unknown query")
	var logs := await _call("editor_logs")
	var texts: Array[String] = []
	for entry in logs.get("entries", []):
		texts.append(String(entry.get("text", "")))
	assert_true("\n".join(texts).contains("listening"), str(logs))


## S13 A4, the request table on the wire: editor_request's kind enum is every token the table
## serves (S13 A5: the catalog query's request kinds but those a person answers, the pickers),
## each a kind the reader knows (a member no kind takes is refused as that member, never as an
## unknown kind), the retail token gone; a field outside a kind's set is refused naming what it
## takes, and one it must carry left out is refused. The edits' schema is the session's batch table
## (S13 D9: the catalog's batch): its op enum the table's ops, apply among them, one property per
## member, an apply's payload the one token it takes and a span's line from 1. The game install's words: the settings take
## game_install and play_in_install (the retail keys refused), the run section says in_install
## and game_install, the import section install_files, and an import from an install that holds
## no archives is import.install.
func test_request_table_on_the_wire() -> void:
	if _client == null:
		return
	var listed: Variant = await _client.rpc(get_tree(), "tools/list")
	var kinds: Array = []
	var queries: Array = []
	var edit: Dictionary = {}
	for tool in (listed as Dictionary).get("result", {}).get("tools", []):
		if String(tool["name"]) == "editor_request":
			kinds = tool["inputSchema"]["properties"]["kind"].get("enum", [])
			edit = tool["inputSchema"]["properties"]["edits"].get("items", {})
		if String(tool["name"]) == "editor_query":
			queries = tool["inputSchema"]["properties"]["query"].get("enum", [])
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
	assert_false(served.has("pick_directory") or served.has("pick_file"), "the pickers need a person")
	assert_true(served.has("preview_install_import") and not served.has("preview_retail_import"))
	for kind: Variant in served:
		var error := String((await _call("editor_request", {"kind": kind, "zzz": 1})).get("_error", ""))
		assert_true(error.contains("zzz") and not error.contains("Unknown request kind"), "%s: %s" % [kind, error])
	assert_true(String((await _call("editor_request", {"kind": "preview_retail_import"})).get("_error", "")).contains(
			"Unknown request kind"), "the retail token names nothing")
	assert_true(String((await _call("editor_request", {"kind": "build", "path": "x"})).get("_error", "")).contains(
			"build takes no \"path\" (it takes out_dir)"))
	assert_true(String((await _call("editor_request", {"kind": "open_project"})).get("_error", "")).contains(
			"needs \"dir\""))
	for retired in ["text", "flag", "edit", "unsaved_choice"]:
		assert_true(String((await _call("editor_request", {"kind": "save", retired: true})).get("_error", "")).contains(
				"Unknown request member"), "%s names no field" % retired)

	var dir := OS.get_cache_dir().path_join("opennova request table %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Words"})).get("ok", false)))
	var install := dir.path_join("no install here")
	var applied := await _call("editor_request", {"kind": "apply_project_settings",
			"settings": {"serial": 1, "game_install": install, "play_in_install": true}})
	assert_true(bool(applied.get("ok", false)) and bool(applied.get("outcome", {}).get("done", false)), str(applied))
	var state := await _state(["run", "import"])
	var play: Dictionary = state.get("run", {})
	assert_true(bool(play.get("in_install", false)), str(play))
	assert_true(String(play.get("game_install", "")).ends_with("no install here"), str(play))
	assert_false(play.has("retail") or play.has("retail_directory"), str(play))
	var import: Dictionary = state.get("import", {})
	assert_true(import.has("install_files") and not import.has("retail_files"), str(import))
	assert_true(String((await _call("editor_request", {"kind": "apply_project_settings",
			"settings": {"retail_directory": install}})).get("_error", "")).contains("retail_directory"))
	var listing := await _call("editor_request", {"kind": "preview_install_import", "with_dependencies": true})
	var findings: Array = listing.get("outcome", {}).get("findings", [])
	assert_true(not findings.is_empty() and String(findings[0].get("code", "")) == "import.install", str(listing))


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
	# The preview (headless): the screen as the game would draw it, LATER where it was put
	# and the title's new text; the game's hit test at LATER's centre finds it; the
	# options hold it under the mouse.
	var preview := await _call("editor_menu_preview", {"op": "state"})
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_eq(String(preview.get("screen", {}).get("name", "")), "STARTUP")
	assert_gt(int(preview.get("widget_count", 0)), 3)
	var rects := await _call("editor_menu_preview", {"op": "rects"})
	var by_name := {}
	for widget: Variant in rects.get("widgets", []):
		by_name[String(widget["name"])] = widget
	assert_eq(String(by_name.get("TITLE", {}).get("text", "")), "John Smith's Game", str(rects))
	var later_rect: Array = by_name.get("LATER", {}).get("rect", [])
	assert_eq(later_rect.size(), 4, str(rects))
	if later_rect.size() == 4:
		assert_eq(int(later_rect[0]), 340)
		var hit := await _call("editor_menu_preview", {"op": "hit",
				"x": (float(later_rect[0]) + float(later_rect[2])) / 2.0,
				"y": (float(later_rect[1]) + float(later_rect[3])) / 2.0})
		assert_eq(int(hit.get("id", 0)), later, str(hit))
	var held := await _call("editor_menu_preview", {"op": "options", "force_id": later, "force_state": "mouseover"})
	assert_eq(int(held.get("options", {}).get("force_id", 0)), later, str(held))
	assert_eq(String(held.get("options", {}).get("force_state", "")), "mouseover", str(held))
	# LATER dragged by its move handle onto the grid, then nudged a unit: two undo steps.
	var dragged := await _call("editor_menu_preview", {"op": "drag", "id": later, "handle": "move", "dx": 13, "dy": 5})
	assert_eq(String(dragged.get("status", "")), "ready", str(dragged))
	assert_eq(int(_rect_of(await _call("editor_menu_preview", {"op": "rects", "limit": 200}), "LATER")[0]), 352)
	var nudged := await _call("editor_menu_preview", {"op": "nudge", "id": later, "dx": 1, "dy": 0})
	assert_eq(String(nudged.get("status", "")), "ready", str(nudged))
	assert_eq(int(_rect_of(await _call("editor_menu_preview", {"op": "rects", "limit": 200}), "LATER")[0]), 353)
	assert_true((await _call("editor_menu_preview", {"op": "drag", "id": later, "handle": "middle", "dx": 1, "dy": 1})).has("_error"),
			"an unknown handle")
	assert_true((await _call("editor_menu_preview", {"op": "nudge", "id": later})).has("_error"), "nudge needs dx and dy")
	assert_true((await _call("editor_menu_preview", {"op": "drag", "id": 999999, "handle": "move", "dx": 1, "dy": 1})).has("_error"),
			"a record the preview does not show")
	for _step in 2:
		assert_true(_done(await _ask("undo")))
	assert_eq(int(_rect_of(await _call("editor_menu_preview", {"op": "rects", "limit": 200}), "LATER")[0]), 340,
			"two undo steps put it back")
	assert_true((await _call("editor_menu_preview", {"op": "hit"})).has("_error"), "hit needs x and y")
	assert_true((await _call("editor_menu_preview", {"op": "nope"})).has("_error"))
	# The frame compiler's notes on the preview, and the render check's headless render of
	# the same screen (the menu_render query): the same windows.
	var notes := await _call("editor_menu_preview", {"op": "notes"})
	assert_eq(String(notes.get("status", "")), "ready", str(notes))
	assert_eq(int(notes.get("count", -1)), (notes.get("notes", []) as Array).size(), str(notes))
	var render := await _query("menu_render", {"path": String(preview.get("path", "")),
			"screen": int(preview.get("screen", {}).get("id", 0)), "limit": 200})
	assert_eq(String(render.get("status", "")), "ready", str(render))
	assert_eq(int(render.get("widget_count", 0)), int(preview.get("widget_count", -1)), str(render))
	var render_names := {}
	for widget: Variant in render.get("widgets", []):
		render_names[String(widget["name"])] = widget
	assert_true(render_names.has("LATER"), str(render))
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
	var waiting := await _call("editor_build")
	assert_true(waiting.get("_error", "").contains("resolve_unsaved"), str(waiting))
	var prompt: Dictionary = (await _state(["dialogs"])).get("dialogs", {}).get("unsaved_prompt", {})
	assert_true(bool(prompt.get("open", false)), str(prompt))
	assert_eq(String(prompt.get("action", "")), "build")
	assert_eq(prompt.get("files", []).size(), 2, str(prompt))
	assert_false(bool(prompt.get("can_discard", true)))
	var saved := await _call("editor_request", {"kind": "resolve_unsaved", "choice": "save"})
	assert_true(bool(saved.get("outcome", {}).get("done", false)), str(saved))
	var built := await _call("editor_build")
	assert_true(bool(built.get("ok", false)), str(built))
	assert_true(FileAccess.file_exists(String(built.get("dir", "")).path_join("localres.pff")), str(built))

	# Play: the game runs on the build with its own endpoint, which the game tools drive.
	# It starts the game only on Windows (godot/src/authoring/child_process.h): elsewhere
	# it refuses and says so, and there is no running game to drive.
	if OS.get_name() != "Windows":
		var refused := await _call("editor_play", {"op": "start"})
		assert_ne(String(refused.get("state", "")), "running", str(refused))
		pending("Play is Windows-only (godot/src/authoring/child_process.h)")
		return
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--max-fps", "60"]))
	var play := await _call("editor_play", {"op": "start"})
	assert_eq(String(play.get("state", "")), "running", str(play))
	var port := int(play.get("mcp_port", 0))
	assert_gt(port, 0)
	# S13 A8: the game runs in a run directory of its own, its log there, the build only read.
	var run_dir := String(play.get("run_dir", ""))
	assert_true(run_dir.ends_with("/.opennova/run/1"), str(play))
	assert_eq(String(play.get("log_file", "")), run_dir.path_join("session.log"), str(play))
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
		last = await _call("editor_play", {"op": "state"})
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
	assert_gt(row_list.size(), 11, str(rows))
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
	var built := await _call("editor_build")
	assert_true(bool(built.get("ok", false)), str(built))


## The asset graph through the endpoint (S7): a blank project's references all resolve, a
## texture named by the menu is found from both ends, the rename rewrites the menu and
## moves the file, and a rename that would break a site the editor cannot rewrite is
## refused with the reason.
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
	var fonts := await _query("symbols", {"kind": "style_var"})
	assert_gt(int(fonts.get("count", 0)), 0)
	# S12 Z2: the graph's lists page as search does: the total as count, a page of at most limit.
	var all_symbols := await _query("symbols")
	assert_gt(int(all_symbols.get("count", 0)), 2, str(all_symbols))
	var symbol_page := await _query("symbols", {"offset": 1, "limit": 2})
	assert_eq(int(symbol_page.get("count", -1)), int(all_symbols.get("count", 0)))
	assert_eq(int(symbol_page.get("offset", -1)), 1)
	assert_eq(symbol_page.get("symbols", []).size(), 2, str(symbol_page))
	if symbol_page.get("symbols", []).size() == 2:
		assert_eq(String(symbol_page["symbols"][0].get("locator", "")), String(all_symbols["symbols"][1].get("locator", "")))
	var edge_page := await _query("references", {"path": "main.mnu", "limit": 1})
	assert_eq(edge_page.get("edges", []).size(), 1, str(edge_page))
	assert_eq(int(edge_page.get("count", 0)), int(references.get("count", -1)))
	assert_eq(String(edge_page.get("path", "")), "main.mnu")
	assert_true((await _query("missing", {"limit": 0})).has("_error"), "a limit from 1")
	assert_true((await _query("usages", {"path": "menu_style.mns", "offset": -1})).has("_error"), "an offset from 0")
	# A texture the menu names: a 4x4 TGA (the menu opens on its first screen, which the
	# preview draws, decoding it; Godot's reader wants more bytes than a 1x1 file has).
	var root: String = (await _state(["project"]))["project"]["root"]
	var texture := FileAccess.open(root.path_join("logo.tga"), FileAccess.WRITE)
	assert_not_null(texture)
	var tga := PackedByteArray([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 4, 0, 32, 8])
	var pixels := PackedByteArray()
	pixels.resize(4 * 4 * 4)
	pixels.fill(255)
	tga.append_array(pixels)
	texture.store_buffer(tga)
	texture.close()
	assert_true(bool((await _call("editor_request", {"kind": "rescan"})).get("ok", false)))
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	var exit_id := await _find("EXIT")
	assert_gt(exit_id, 0)
	# S12 Z2: a reference field's picker names and its Go to places. MAIN's font names a style
	# variable: the picker offers the project's fonts and the stylesheet's variables; Go to leads
	# to the variable where the game reads it, then to the .fnt its value names (which the
	# editor does not open).
	var main_id := await _find("MAIN", "MAIN.MNU/STARTUP")
	assert_gt(main_id, 0)
	var choices := await _query("reference_choices", {"id": main_id, "field": "font.name"})
	assert_eq(String(choices.get("reference", "")), "font", str(choices))
	var offered := {}
	for choice in choices.get("choices", []):
		offered[String(choice.get("kind", ""))] = true
		if String(choice.get("name", "")) == "%DEF_FONTNAME_LG%":
			assert_eq(String(choice.get("status", "")), "present", str(choice))
	assert_true(offered.has("font") and offered.has("style_var"), str(choices))
	assert_eq(int(choices.get("count", 0)), choices.get("choices", []).size(), "one page holds them")
	var second := await _query("reference_choices", {"id": main_id, "field": "font.name", "offset": 1, "limit": 1})
	assert_eq(second.get("choices", []).size(), 1, str(second))
	assert_eq(int(second.get("count", -1)), int(choices.get("count", 0)))
	var targets := await _query("reference_targets", {"id": main_id, "field": "font.name"})
	assert_eq(String(targets.get("value", "")), "%DEF_FONTNAME_LG%", str(targets))
	var places: Array = targets.get("targets", [])
	assert_eq(places.size(), 2, str(targets))
	if places.size() == 2:
		assert_true(String(places[0].get("file", "")).ends_with("menu_style.mns"), str(places[0]))
		assert_true(bool(places[0].get("editable", false)) and not String(places[0].get("locator", "")).is_empty())
		assert_true(String(places[1].get("file", "")).to_lower().ends_with(".fnt"), str(places[1]))
		assert_false(bool(places[1].get("editable", true)), "Files shows a font")
	assert_eq(int((await _query("reference_choices", {"id": main_id, "field": "name"})).get("count", -1)), 0,
			"a field that is no reference offers nothing")
	assert_true((await _query("reference_targets", {"id": main_id, "field": "nope"})).has("_error"))
	assert_true((await _query("reference_choices", {"id": main_id})).has("_error"), "choices needs a field")
	# S12 D3: a window found on its own screen; who uses the stylesheet (no file names it, the
	# menus name its variables), each use with the locator of the record that makes it.
	assert_eq(await _find("EXIT", "MAIN.MNU/STARTUP"), exit_id)
	assert_true((await _query("record", {"symbol": "EXIT", "scope": "MAIN.MNU/NOWHERE"})).has("_error"))
	# S12 D8: find in the open menu (every field whose value as shown holds the text, without
	# case) and in the project (the files and symbols whose names hold it, with their usages).
	var hits := await _query("document_search", {"text": "exit"})
	var exit_hit := false
	for hit in hits.get("hits", []):
		exit_hit = exit_hit or (int(hit.get("id", 0)) == exit_id and String(hit.get("field", "")) == "name"
				and not String(hit.get("locator", "")).is_empty())
	assert_true(exit_hit, str(hits))
	for hit in (await _query("document_search", {"text": "exit", "match_case": true})).get("hits", []):
		assert_false(int(hit.get("id", 0)) == exit_id and String(hit.get("field", "")) == "name", "match_case keeps EXIT out")
	assert_true((await _query("document_search")).has("_error"), "search needs a text")
	var found_menu := false
	var found_exit := false
	for hit in (await _query("project_search", {"text": "main.mnu"})).get("hits", []):
		found_menu = found_menu or (String(hit.get("kind", "")) == "file" and String(hit.get("name", "")) == "main.mnu")
	for hit in (await _query("project_search", {"text": "exi"})).get("hits", []):
		found_exit = found_exit or (String(hit.get("kind", "")) == "menu_window" and String(hit.get("name", "")) == "EXIT"
				and hit.has("usages"))
	assert_true(found_menu and found_exit)
	# A page at a time: the total as count, the page's hits alone.
	var everything := await _query("project_search", {"text": "e"})
	var page := await _query("project_search", {"text": "e", "offset": 1, "limit": 1})
	assert_gt(int(everything.get("count", 0)), 1, str(everything))
	assert_eq(int(page.get("count", -1)), int(everything.get("count", 0)))
	assert_eq(page.get("hits", []).size(), 1, str(page))
	assert_true((await _query("project_search", {"text": "e", "limit": 0})).has("_error"))
	var document_page := await _query("document_search", {"text": "exit", "limit": 1})
	assert_eq(document_page.get("hits", []).size(), 1, str(document_page))
	assert_eq(int((await _query("referrers", {"path": "menu_style.mns"})).get("count", -1)), 0)
	var style_uses := await _query("usages", {"path": "menu_style.mns"})
	var style_edges: Array = style_uses.get("edges", [])
	assert_false(style_edges.is_empty(), str(style_uses))
	if not style_edges.is_empty():
		assert_false(String(style_edges[0].get("locator", "")).is_empty(), str(style_edges[0]))
	# EXIT's first APPEARANCE row made an image (the rows are records of their own).
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
	assert_eq(String(users["edges"][0].get("field", "")), "value")
	assert_eq(String(users["edges"][0].get("record", "")), "STARTUP/MAIN/EXIT/Appearance 1")
	# The menu that names it has unsaved edits: the rename waits on the unsaved prompt, which
	# names the rename and the menu and offers no discard; its save writes the menu and renames.
	var waiting := await _call("editor_request", {"kind": "rename_asset", "path": "logo.tga", "new_name": "logo2.tga"})
	assert_true(bool(waiting.get("ok", false)) and bool(waiting.get("outcome", {}).get("unsaved_prompt", false)),
			str(waiting))
	assert_false(_done(waiting), "a rename that waits on the prompt is not done")
	var prompt: Dictionary = (await _state(["dialogs"])).get("dialogs", {}).get("unsaved_prompt", {})
	assert_eq(String(prompt.get("action", "")), "rename_asset", str(prompt))
	assert_eq(prompt.get("files", []).size(), 1, str(prompt))
	assert_false(bool(prompt.get("can_discard", true)))
	assert_true(FileAccess.file_exists(root.path_join("logo.tga")))
	var renamed := await _call("editor_request", {"kind": "resolve_unsaved", "choice": "save"})
	assert_true(bool(renamed.get("outcome", {}).get("done", false)), str(renamed))
	assert_false(FileAccess.file_exists(root.path_join("logo.tga")))
	assert_true(FileAccess.file_exists(root.path_join("logo2.tga")))
	var reloaded_exit := await _record(await _find("EXIT"))
	var reloaded_row := 0
	for collection in reloaded_exit.get("collections", []):
		if String(collection.get("kind_name", "")) == "appearance":
			reloaded_row = int(collection["records"][0]["id"])
	assert_eq(String(await _value(reloaded_row, "value")), "logo2.tga")
	assert_eq(int((await _query("missing")).get("count", -1)), 0)
	var extension := await _call("editor_request", {"kind": "rename_asset", "path": "logo2.tga", "new_name": "logo2.pcx"})
	assert_true(bool(extension.get("ok", false)) and not _done(extension), "the extension stays: " + str(extension))
	# The new name taken on disk after the last scan: the commit refuses, the outcome says why,
	# and the Problems row outlives the refresh that follows.
	var late := FileAccess.open(root.path_join("late.tga"), FileAccess.WRITE)
	assert_not_null(late)
	late.store_string("late")
	late.close()
	var taken := await _call("editor_request", {"kind": "rename_asset", "path": "logo2.tga", "new_name": "late.tga"})
	assert_true(not _done(taken) and _found(taken, "rename.exists"), str(taken))
	assert_true(FileAccess.file_exists(root.path_join("logo2.tga")))
	var codes: Array[String] = []
	for problem in (await _query("problems", {"severities": ["error"]})).get("problems", []):
		codes.append(String(problem.get("code", "")))
	assert_has(codes, "rename.exists")
	# Assigning a requirement that is already met renames nothing, and the outcome says so.
	var assigned := await _call("editor_request", {"kind": "assign_requirement", "role": "main_menu", "path": "logo2.tga"})
	assert_true(not _done(assigned) and _found(assigned, "requirement.assigned"), str(assigned))
	assert_true(FileAccess.file_exists(root.path_join("logo2.tga")))
	# S12 D9: a style variable the blank menu names renamed everywhere: planned first
	# (preview_rename, the dialogs section's rename_preview: the definition and its uses, each
	# before and after), then committed on disk; every use still resolves, under the new name.
	var variable: Dictionary = {}
	for symbol in (await _query("symbols", {"kind": "style_var"})).get("symbols", []):
		if not bool(symbol.get("inert", false)) and String(symbol.get("name", "")) == "DEF_FONTNAME_LG":
			variable = symbol
	assert_false(variable.is_empty(), "DEF_FONTNAME_LG is defined where the game reads it")
	var rename_args := {"path": String(variable.get("file", "")), "locator": String(variable.get("locator", "")),
			"field": String(variable.get("field", "")), "new_name": "DEF_FONTNAME_BIG"}
	var previewed := await _call("editor_request", {"kind": "preview_rename"}.merged(rename_args))
	assert_true(_done(previewed), str(previewed))
	var planned: Dictionary = (await _state(["dialogs"])).get("dialogs", {}).get("rename_preview", {})
	assert_true(bool(planned.get("ok", false)), str(planned))
	var sites: Array = planned.get("sites", [])
	assert_gt(sites.size(), 0, str(planned))
	if not sites.is_empty():
		assert_eq(String(sites[0].get("before", "")), "DEF_FONTNAME_LG")
		assert_eq(String(sites[0].get("after", "")), "DEF_FONTNAME_BIG")
	var committed := await _call("editor_request", {"kind": "rename_symbol"}.merged(rename_args))
	assert_true(bool(committed.get("outcome", {}).get("done", false)), str(committed))
	var big := false
	for symbol in (await _query("symbols", {"kind": "style_var"})).get("symbols", []):
		big = big or String(symbol.get("name", "")) == "DEF_FONTNAME_BIG"
	assert_true(big, "the variable renamed")
	assert_eq(int((await _query("missing")).get("count", -1)), 0)
	assert_true((await _call("editor_request", {"kind": "rename_symbol", "path": "menu_style.mns"})).has("_error"),
			"rename_symbol needs a locator, a field and a name")


## A request's outcome through the endpoint (S9b): `ok` says the request read, the outcome
## says whether it happened. A badly named new document is refused with its finding and
## writes nothing; a new menu of its own gets one screen named after the file; closing it
## with unsaved changes waits on the prompt, which the outcome says.
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
	assert_false(bool(bad.get("outcome", {}).get("done", true)), str(bad))
	var findings: Array = bad.get("outcome", {}).get("findings", [])
	assert_eq(findings.size(), 1, str(bad))
	if findings.size() == 1:
		assert_eq(String(findings[0].get("code", "")), "document.name")
	assert_false(FileAccess.file_exists(dir.path_join("x.mnu")))
	var wrong := await _call("editor_request", {"kind": "create_file", "path": "foo.mnu", "file_kind": "strings"})
	assert_true(not _done(wrong) and _found(wrong, "document.kind"), str(wrong))
	var long := await _call("editor_request", {"kind": "create_file", "path": "abcdefghijklm.mnu"})
	assert_true(not _done(long) and _found(long, "document.name"), str(long))
	var extra := await _call("editor_request", {"kind": "create_file", "path": "extra.mnu", "file_kind": "menu"})
	assert_true(_done(extra), str(extra))
	assert_eq(String((await _query("document", {"path": "extra.mnu"})).get("kind", "")), "menu")
	var rows := await _query("document", {"path": "extra.mnu"})
	assert_eq(int(rows.get("row_count", 0)), 1, str(rows))
	assert_eq(String(rows.get("rows", [{}])[0].get("name", "")), "EXTRA")
	assert_true((await _query("record", {"symbol": "EXIT"})).has("_error"), "no Exit button")
	var main := await _find("MAIN")
	assert_gt(main, 0)
	assert_true(_done(await _set_field(main, "position.left", 8)))
	var closing := await _call("editor_request", {"kind": "close_document", "path": "extra.mnu"})
	assert_true(bool(closing.get("outcome", {}).get("unsaved_prompt", false)) and not _done(closing), str(closing))
	var discarded := await _call("editor_request", {"kind": "resolve_unsaved", "choice": "discard"})
	assert_true(bool(discarded.get("outcome", {}).get("done", false)), str(discarded))
	var listed := await _query("documents")
	for document in listed.get("documents", []):
		assert_ne(String(document.get("path", "")), "menus/extra.mnu", "discarded and closed")


## Every scanned file is reachable past the result's list cap (PAGE_MAX): the files query pages
## the list, count the total, and the project section counts it.
func test_file_list_pages_past_the_cap() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova files mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Files"})).get("ok", false)))
	var extra := EditorMcpCatalog.PAGE_MAX + 10
	assert_eq(DirAccess.make_dir_recursive_absolute(dir.path_join("notes")), OK)
	for i in extra:
		var file := FileAccess.open(dir.path_join("notes/n%03d.txt" % i), FileAccess.WRITE)
		assert_not_null(file)
		if file != null:
			file.store_string("note")
			file.close()
	assert_true(bool((await _call("editor_request", {"kind": "rescan"})).get("ok", false)))
	var first := await _query("files", {"limit": EditorMcpCatalog.PAGE_MAX})
	var total := int(first.get("count", 0))
	assert_gt(total, EditorMcpCatalog.PAGE_MAX, str(first.keys()))
	assert_eq(first.get("files", []).size(), EditorMcpCatalog.PAGE_MAX)
	var names := {}
	var offset := 0
	while offset < total:
		var page := await _query("files", {"offset": offset, "limit": EditorMcpCatalog.PAGE_MAX})
		assert_eq(int(page.get("offset", -1)), offset, str(page.keys()))
		var files: Array = page.get("files", [])
		assert_false(files.is_empty(), "every page up to count holds files")
		if files.is_empty():
			break
		for file in files:
			names[String(file["name"])] = true
		offset += files.size()
	assert_eq(names.size(), total, "every file listed once")
	for i in extra:
		assert_true(names.has("n%03d.txt" % i), "n%03d.txt listed" % i)
	var last := await _query("files", {"offset": total - 1, "limit": 5})
	assert_eq(int(last.get("count", 0)), total)
	assert_eq(int(last.get("offset", -1)), total - 1)
	assert_eq(last.get("files", []).size(), 1, "the last page of the files")
	assert_eq(last.get("next_offset"), null, "no page after the last")
	assert_eq(int((await _state(["project"])).get("project", {}).get("file_count", 0)), total)


## The image importer through the endpoint (S8): a PNG imported into the project becomes a
## PCX under the cache with a committed sidecar beside the source, the state lists the import,
## the graph resolves the output as a texture, and the source itself is never a build member.
## A PNG saved into the project with no record is a texture the game loads as it is (S9p2a).
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
		assert_eq(String(imported[0].get("source", "")), "logo.png")
		assert_true(bool(imported[0].get("ok", false)))
		assert_eq(imported[0].get("outputs", []).size(), 1)
		assert_true(FileAccess.file_exists(root.path_join(String(imported[0]["outputs"][0]))))
		assert_eq(imported[0].get("inputs", null), [], "the image importer reads its source alone")
	assert_true(FileAccess.file_exists(root.path_join("logo.png.import")), "importing writes the record beside the source")
	var problems := await _query("problems", {"severities": ["error"]})
	assert_eq(int(problems.get("shown", -1)), 0, str(problems))
	var symbols := await _query("referrers", {"path": "logo.pcx"})
	assert_eq(int(symbols.get("count", -1)), 0)
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


## S9k2 through the endpoint: TITLE and EXIT copied from STARTUP and pasted into a second
## screen's MAIN and into another menu file (their names free there), the selection
## duplicated in one step and undone, the two arranged by editor_menu_preview op=arrange and
## undone, and the refusals.
func test_clipboard_across_screens_and_arrange() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova clipboard mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Clip"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	var menu := await _open("main.mnu")
	assert_eq(String(menu.get("kind", "")), "menu", str(menu))
	var title := await _find("TITLE")
	var exit := await _find("EXIT")
	assert_gt(title, 0)
	assert_gt(exit, 0)
	# Opened, the menu shows its first screen (the screen selected, none of its windows);
	# with the selection toggled empty there is nothing to duplicate.
	var opened: Dictionary = (await _state(["selection"])).get("selection", {})
	assert_gt(int(opened["primary"]["row"]), 0, str(opened))
	assert_eq(int(opened["primary"]["child"]), 0, str(opened))
	assert_true(bool((await _select(title)).get("ok", false)))
	assert_true(bool((await _select(title, "toggle")).get("ok", false)))
	var nothing := await _ask("duplicate")
	assert_true(bool(nothing.get("ok", false)) and not _done(nothing), "nothing selected to duplicate: " + str(nothing))

	# A second screen, with its own MAIN.
	var second := await _add("screen")
	assert_gt(second, 0)
	var rows := await _query("document")
	var second_main := 0
	for row: Variant in rows.get("rows", []):
		if row is Dictionary and int((row as Dictionary).get("id", 0)) == second:
			second_main = int((row as Dictionary)["collections"][0]["records"][0]["id"])
	assert_gt(second_main, 0, str(rows))

	# TITLE and EXIT copied, pasted into the second screen's MAIN.
	assert_true(bool((await _select(title)).get("ok", false)))
	assert_true(bool((await _select(exit, "add")).get("ok", false)))
	assert_true(_done(await _ask("copy")))
	assert_gt(int((await _state(["selection"])).get("selection", {}).get("clipboard_bytes", 0)), 0)
	var pasted := await _ask("paste", {"paste_at": {"parent": second_main}})
	assert_true(_done(pasted), str(pasted))
	var selected: Array = (await _state(["selection"])).get("selection", {}).get("records", [])
	assert_eq(selected.size(), 2, str(selected))
	if selected.size() == 2:
		var copy_id := int((selected[0] as Dictionary).get("child", 0))
		var copy_record := await _record(copy_id)
		assert_true(String(copy_record.get("path", "")).ends_with("/MAIN/TITLE"), str(copy_record))
		assert_eq(int((copy_record.get("owner", {}) as Dictionary).get("row", 0)), second, str(copy_record))

	# Into another menu file.
	var extra := await _call("editor_request", {"kind": "create_file", "path": "extra.mnu", "file_kind": "menu"})
	assert_true(_done(extra), str(extra))
	assert_eq(String((await _query("document", {"path": "extra.mnu"})).get("kind", "")), "menu")
	var extra_main := await _find("MAIN")
	assert_gt(extra_main, 0)
	var into_extra := await _ask("paste", {"paste_at": {"parent": extra_main}})
	assert_true(_done(into_extra), str(into_extra))
	assert_eq((await _state(["selection"])).get("selection", {}).get("records", []).size(), 2, str(into_extra))
	assert_gt(await _find("EXIT"), 0)

	# Back in main.mnu: the selection duplicated in one step, each copy after its original.
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	assert_true(bool((await _select(title)).get("ok", false)))
	assert_true(bool((await _select(exit, "add")).get("ok", false)))
	var duplicated := await _ask("duplicate")
	assert_true(_done(duplicated), str(duplicated))
	assert_eq((await _state(["selection"])).get("selection", {}).get("records", []).size(), 2, str(duplicated))
	assert_gt(await _find("EXIT2"), 0)
	assert_true(_done(await _ask("undo")))
	assert_true((await _query("record", {"symbol": "EXIT2"})).has("_error"), "the duplicate undone")

	# Arranged in the preview: EXIT's left edge to TITLE's, one undo step.
	var arranged := await _call("editor_menu_preview", {"op": "arrange", "ids": [title, exit], "arrange": "align_left"})
	assert_eq(String(arranged.get("status", "")), "ready", str(arranged))
	var rects := await _call("editor_menu_preview", {"op": "rects", "limit": 200})
	assert_eq(int(_rect_of(rects, "EXIT")[0]), int(_rect_of(rects, "TITLE")[0]), str(rects))
	assert_true(_done(await _ask("undo")))
	rects = await _call("editor_menu_preview", {"op": "rects", "limit": 200})
	assert_eq(int(_rect_of(rects, "EXIT")[0]), 340, str(rects))
	assert_true((await _call("editor_menu_preview", {"op": "arrange", "ids": [title], "arrange": "align_left"})).has("_error"),
			"one window cannot be aligned")
	assert_true((await _call("editor_menu_preview", {"op": "arrange", "ids": [title, exit], "arrange": "sideways"})).has("_error"))
	assert_true((await _call("editor_menu_preview", {"op": "arrange", "arrange": "align_left"})).has("_error"), "ids are required")


## A window of a menu_tree answer by name (an empty Dictionary when it is not there).
func _tree_window(tree: Dictionary, name: String) -> Dictionary:
	for screen: Variant in tree.get("screens", []):
		if not (screen is Dictionary):
			continue
		for window: Variant in (screen as Dictionary).get("windows", []):
			if window is Dictionary and String((window as Dictionary).get("name", "")) == name:
				return window as Dictionary
	return {}


## S9m through the endpoint, as queries and edit_record's batch form (S13 A5): the menu_tree of
## STARTUP (a closed menu as the last validation read it, then the open one), a button with two
## ACTIONs and a SOUND and a list with an ITEM added and filled in by label in one batch (the ids
## answered in the outcome's made and added, the tree and the preview showing them, one undo step
## taking all of it), a button's ACTIONs replaced by a replace_list edit and undone, a record's
## fields set in the order the client wrote them, the menu_findings by source (the sound bank the
## project lacks is the graph's), paging, and the refusals (an unknown label before the document
## sees anything, a field the document refuses with nothing committed, a path naming no menu, an
## unknown query or menu).
func test_menu_tools_through_the_editor_mcp() -> void:
	if _client == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova menu tools mcp %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(bool((await _call("editor_request", {"kind": "new_project", "dir": dir, "title": "Tools"})).get("ok", false)))
	assert_true(bool((await _create_missing()).get("ok", false)))
	# Closed: the render check's copy, every window placed.
	var closed := await _query("menu_tree", {"path": "main.mnu"})
	assert_false(bool(closed.get("open", true)), str(closed))
	assert_eq(closed.get("screens", []).size(), 1, str(closed))
	var title_closed := _tree_window(closed, "TITLE")
	assert_eq(String(title_closed.get("text", "")), "Tools", str(closed))
	assert_eq((title_closed.get("rect", []) as Array).size(), 4, str(title_closed))
	# Open: the ids a batch names.
	assert_eq(String((await _open("main.mnu")).get("kind", "")), "menu")
	var tree := await _query("menu_tree")
	assert_true(bool(tree.get("open", false)), str(tree))
	var main := int(_tree_window(tree, "MAIN").get("id", 0))
	assert_gt(main, 0, str(tree))
	var screen: int = int(((tree.get("screens", [{}]) as Array)[0] as Dictionary).get("id", 0))
	assert_gt(screen, 0)
	var windows_before: int = int(((tree.get("screens", [{}]) as Array)[0] as Dictionary).get("window_count", 0))

	var edits: Array = [
		{"op": "add", "kind": "window", "parent": main, "as": "hello"},
		{"op": "set", "id": "hello", "field": "name", "value": "HELLO"},
		{"op": "set", "id": "hello", "field": "type", "value": "BUTTON"},
		{"op": "set", "id": "hello", "field": "string.value", "value": "Hello"},
		{"op": "set", "id": "hello", "field": "position.left", "value": 340},
		{"op": "set", "id": "hello", "field": "position.top", "value": 430},
		{"op": "set", "id": "hello", "field": "position.right", "value": 460},
		{"op": "add", "kind": "action", "parent": "hello", "as": "back"},
		{"op": "add", "kind": "action", "parent": "hello", "as": "show"},
		{"op": "set", "id": "show", "field": "type", "value": "WINDOW"},
		{"op": "set", "id": "show", "field": "state", "value": "SHOW"},
		{"op": "set", "id": "show", "field": "target", "value": "TITLE"},
		{"op": "add", "kind": "sound", "parent": "hello", "as": "click"},
		{"op": "set", "id": "click", "field": "file", "value": "menu.lwf"},
		{"op": "add", "kind": "window", "parent": main, "as": "list"},
		{"op": "set", "id": "list", "field": "name", "value": "CHOICES"},
		{"op": "set", "id": "list", "field": "type", "value": "LIST"},
		{"op": "set", "id": "list", "field": "position.left", "value": 340},
		{"op": "set", "id": "list", "field": "position.top", "value": 470},
		{"op": "set", "id": "list", "field": "position.right", "value": 460},
		{"op": "set", "id": "list", "field": "position.bottom", "value": 520},
		{"op": "add", "kind": "items.item", "parent": "list", "as": "one"},
		{"op": "set", "id": "one", "field": "text", "value": "One"},
	]
	var batch := await _edit(edits, "main.mnu")
	assert_true(bool(batch.get("outcome", {}).get("done", false)), str(batch))
	var made: Dictionary = batch.get("outcome", {}).get("made", {})
	assert_eq(made.size(), 6, str(batch))
	assert_eq((batch.get("outcome", {}).get("added", []) as Array).size(), 6, str(batch))
	var hello := int(made.get("hello", 0))
	assert_gt(hello, 0)
	assert_eq(await _find("HELLO"), hello)
	var show := await _record(int(made.get("show", 0)))
	var show_fields := {}
	for field: Variant in show.get("fields", []):
		show_fields[String((field as Dictionary).get("id", ""))] = (field as Dictionary).get("value", "")
	assert_eq(String(show_fields.get("type", "")), "WINDOW", str(show))
	assert_eq(String(show_fields.get("target", "")), "TITLE", str(show))

	# The tree and the preview show them.
	tree = await _query("menu_tree")
	var hello_window := _tree_window(tree, "HELLO")
	assert_eq(String(hello_window.get("type", "")), "BUTTON", str(hello_window))
	assert_eq(String(hello_window.get("text", "")), "Hello", str(hello_window))
	assert_eq(int(hello_window.get("parent", 0)), main)
	assert_eq(int((hello_window.get("lists", {}) as Dictionary).get("action", 0)), 2, str(hello_window))
	assert_eq(int((hello_window.get("lists", {}) as Dictionary).get("sound", 0)), 1, str(hello_window))
	var local: Array = hello_window.get("local", [])
	assert_eq(local.size(), 4, str(hello_window))
	if local.size() == 4:
		assert_eq(int(local[0]), 340)
		assert_eq(int(local[1]), 430)
		assert_eq(int(local[2]), 460)
	assert_eq(int((_tree_window(tree, "CHOICES").get("lists", {}) as Dictionary).get("items.item", 0)), 1, str(tree))
	var preview := await _call("editor_menu_preview", {"op": "state"})
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	var rects := await _call("editor_menu_preview", {"op": "rects", "limit": 200})
	var hello_rect := _rect_of(rects, "HELLO")
	assert_eq(int(hello_rect[0]), 340, str(rects))
	for widget: Variant in rects.get("widgets", []):
		if widget is Dictionary and String((widget as Dictionary).get("name", "")) == "HELLO":
			assert_eq(String((widget as Dictionary).get("text", "")), "Hello")

	# One undo step takes the whole batch; redo puts it back.
	assert_true(_done(await _ask("undo")))
	assert_true((await _query("record", {"symbol": "HELLO"})).has("_error"), "the batch undone")
	assert_true((await _query("record", {"symbol": "CHOICES"})).has("_error"))
	assert_true(_done(await _ask("redo")))
	hello = await _find("HELLO")
	assert_gt(hello, 0)

	# A list replaced: HELLO's ACTIONs by one, one undo step.
	var listed := await _edit([{"op": "replace_list", "id": hello, "list": "action",
			"records": [{"type": "WINDOW", "state": "HIDE", "target": "TITLE"}]}], "main.mnu")
	assert_true(bool(listed.get("outcome", {}).get("done", false)), str(listed))
	assert_eq((listed.get("outcome", {}).get("added", []) as Array).size(), 1, str(listed))
	tree = await _query("menu_tree")
	assert_eq(int((_tree_window(tree, "HELLO").get("lists", {}) as Dictionary).get("action", 0)), 1, str(tree))
	assert_true(_done(await _ask("undo")))
	tree = await _query("menu_tree")
	assert_eq(int((_tree_window(tree, "HELLO").get("lists", {}) as Dictionary).get("action", 0)), 2, str(tree))
	# A record's fields set in the order written, through the endpoint: a body's draw kind,
	# then its flag cleared, leaves no draw kind (sorted keys would clear the flag first).
	var drawn := await _edit([{"op": "replace_list", "id": hello, "list": "column.body",
			"records": [{"display": "CUSTOM_DRAW", "custom_draw": 0}]}], "main.mnu")
	assert_true(bool(drawn.get("outcome", {}).get("done", false)), str(drawn))
	var drawn_added: Array = drawn.get("outcome", {}).get("added", [])
	assert_eq(drawn_added.size(), 1, str(drawn))
	if drawn_added.size() == 1:
		var drawn_record := await _record(int(drawn_added[0]))
		var drawn_fields := {}
		for field in drawn_record.get("fields", []):
			drawn_fields[String(field["id"])] = field
		assert_eq(String(drawn_fields.get("display", {}).get("value", "?")), "", str(drawn_record))
		assert_eq(int(drawn_fields.get("custom_draw", {}).get("value", -1)), 0, str(drawn_record))
	assert_true(_done(await _ask("undo")))
	# The menu reads name a menu: a stylesheet is none.
	var not_menu := await _query("menu_tree", {"path": "menu_style.mns"})
	assert_true(not_menu.get("_error", "").contains("no menu"), str(not_menu))

	# Refused: an unknown label (nothing asked of the document), a field the document refuses
	# (nothing committed), a list with no records, an unknown query or menu.
	var unknown := await _edit([{"op": "set", "id": "nobody", "field": "name", "value": "X"}], "main.mnu")
	assert_true(unknown.get("_error", "").contains("nobody"), str(unknown))
	var refused := await _edit([
		{"op": "add", "kind": "window", "parent": main, "as": "x"},
		{"op": "set", "id": "x", "field": "no_such_field", "value": 1},
	], "main.mnu")
	assert_true(bool(refused.get("ok", false)) and not _done(refused), str(refused))
	assert_false(refused.get("outcome", {}).get("findings", []).is_empty(), str(refused))
	assert_true((refused.get("outcome", {}).get("added", []) as Array).is_empty(), "nothing added")
	tree = await _query("menu_tree")
	assert_eq(int(((tree.get("screens", [{}]) as Array)[0] as Dictionary).get("window_count", 0)), windows_before + 2,
			"nothing committed")
	assert_true((await _edit([{"op": "replace_list", "id": hello, "list": "action"}], "main.mnu")).has("_error"))
	assert_true((await _call("editor_request", {"kind": "edit_record", "path": "main.mnu"})).has("_error"),
			"edits are required")
	assert_true((await _query("nope")).has("_error"))
	assert_true((await _query("menu_findings", {"path": "nothing.mnu"})).has("_error"))

	# Paging one screen's windows.
	var page := await _query("menu_tree", {"screen": screen, "offset": 1, "limit": 2})
	var paged: Dictionary = (page.get("screens", [{}]) as Array)[0]
	assert_eq((paged.get("windows", []) as Array).size(), 2, str(page))
	assert_eq(int(paged.get("offset", -1)), 1)
	assert_eq(int(paged.get("window_count", 0)), windows_before + 2)
	assert_eq(int(paged.get("count", 0)), windows_before + 2)
	assert_true((await _query("menu_tree", {"screen": 999999})).has("_error"))

	# The findings of the menu: the sound bank the project lacks is the graph's warning.
	var analysis := await _query("menu_findings", {"path": "main.mnu", "limit": 200})
	var bank := false
	for row: Variant in analysis.get("problems", []):
		var problem: Dictionary = row
		bank = bank or (String(problem.get("code", "")) == "reference.missing" and String(problem.get("source", "")) == "graph"
				and String(problem.get("message", "")).contains("menu.lwf"))
	assert_true(bank, str(analysis))
	assert_gt(int((analysis.get("sources", {}) as Dictionary).get("graph", 0)), 0, str(analysis))
	assert_gt(int(((analysis.get("screens", [{}]) as Array)[0] as Dictionary).get("notes", 0)), 0, str(analysis))
	var errors := await _query("menu_findings", {"severity": "error"})
	assert_eq(int(errors.get("count", -1)), (errors.get("problems", []) as Array).size(), str(errors))
