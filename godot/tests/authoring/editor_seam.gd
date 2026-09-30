extends RefCounted

## The editor's typed seam as the GUT tests knew it, over the one wire seam (ADR 0046 S13 A5):
## EditorApp keeps request_json and query_json, and each method here is a request or a query
## through them, answering as the typed method it replaces answered (a record by its identity, a
## field as a Variant of its type, a request's success from its outcome). Test-only: nothing the
## editor ships calls it. Preloaded by path (`preload("res://tests/authoring/editor_seam.gd")`),
## so no class cache stands between a fresh worktree and its tests.

const PAGE_MAX := 200

var app: Node


func _init(editor_app: Node) -> void:
	app = editor_app


# --- the wire -------------------------------------------------------------------------------------

## A request by its wire form: the answer {ok, served, error?, outcome, status, view_revision}, whole
## numbers as integers.
func request(fields: Dictionary) -> Dictionary:
	var answer: Variant = parsed(String(app.call("request_json", JSON.stringify(fields, "", false))))
	return answer if answer is Dictionary else {}


## A request's outcome ({} when it did not read).
func outcome(fields: Dictionary) -> Dictionary:
	var answer := request(fields)
	return answer.get("outcome", {}) if bool(answer.get("ok", false)) else {}


## True when a request read and its outcome is done.
func done(fields: Dictionary) -> bool:
	return bool(outcome(fields).get("done", false))


## A query's answer, or {"error": why} when it was refused.
func query(name: String, args := {}) -> Dictionary:
	var answer: Variant = parsed(String(app.call("query_json", name, JSON.stringify(args))))
	return answer if answer is Dictionary else {"error": "no answer"}


## The state's sections by name.
func state(sections: Array) -> Dictionary:
	return query("state", {"sections": sections})


## Every entry of a paged query's list, page after page.
func every(name: String, key: String, args := {}) -> Array:
	var items: Array = []
	var offset := 0
	while true:
		var page_args := args.duplicate()
		page_args["offset"] = offset
		page_args["limit"] = PAGE_MAX
		var page := query(name, page_args)
		var list: Array = page.get(key, [])
		items.append_array(list)
		if page.get("next_offset") == null or list.is_empty():
			return items
		offset = int(page["next_offset"])
	return items


## The session's JSON as a Variant with whole numbers as integers: Godot's parser reads every
## number as a float, and an id of 3.0 is nobody's idea of an id.
static func parsed(text: String) -> Variant:
	return whole_numbers(JSON.parse_string(text))


static func whole_numbers(value: Variant) -> Variant:
	match typeof(value):
		TYPE_FLOAT:
			return int(value) if float(value) == floorf(float(value)) and absf(float(value)) < 9007199254740992.0 else value
		TYPE_ARRAY:
			var items: Array = []
			for item: Variant in value:
				items.append(whole_numbers(item))
			return items
		TYPE_DICTIONARY:
			var out := {}
			for key: Variant in value:
				out[key] = whole_numbers(value[key])
			return out
		_:
			return value


# --- the project ----------------------------------------------------------------------------------

## Made and opened: the request went through and the project it made is the one open.
func new_project(dir: String, title: String) -> bool:
	return done({"kind": "new_project", "dir": dir, "title": title}) and _open_at(dir)


## Whether the project at `dir` is the one open afterwards (a switch that failed, or that an
## operation refused, leaves the project open before it open).
func open_project(dir: String) -> bool:
	request({"kind": "open_project", "dir": dir})
	return _open_at(dir)


func close_project() -> void:
	request({"kind": "close_project"})


## Every missing required file made (the roles of the unmet rows); how many required files are
## still unmet afterwards.
func create_missing_files() -> int:
	var roles: Array[String] = []
	for row: Variant in state(["requirements"]).get("requirements", {}).get("rows", []):
		if bool(row.get("required", false)) and String(row.get("state", "")) != "present":
			roles.append(String(row["role"]))
	request({"kind": "create_missing", "roles": roles})
	return get_required_missing()


func stop_play() -> void:
	request({"kind": "stop_play"})


func is_project_open() -> bool:
	return bool(_project().get("open", false))


func get_project_title() -> String:
	return String(_project().get("title", ""))


func get_project_root() -> String:
	return String(_project().get("root", ""))


func get_required_missing() -> int:
	var requirements: Dictionary = state(["requirements"]).get("requirements", {})
	return int(requirements.get("missing", 0)) + int(requirements.get("wrong_kind", 0))


func get_required_total() -> int:
	return int(state(["requirements"]).get("requirements", {}).get("total", 0))


func get_last_build_dir() -> String:
	return String(query("operation").get("build", {}).get("dir", ""))


func is_last_build_ok() -> bool:
	var build: Dictionary = query("operation").get("build", {})
	return bool(build.get("has_build", false)) and bool(build.get("ok", false))


func get_play_state() -> String:
	return String(_run().get("state", "stopped"))


func did_game_exit_on_its_own() -> bool:
	return bool(_run().get("exited_on_its_own", false))


func get_play_mcp_port() -> int:
	return int(_run().get("mcp_port", 0))


func get_problem_count() -> int:
	return int(state(["problem_counts"]).get("problem_counts", {}).get("count", 0))


## Every output line held, oldest first.
func get_output_lines() -> PackedStringArray:
	var lines := PackedStringArray()
	var cursor := 0
	while true:
		var page := query("output", {"cursor": cursor, "limit": PAGE_MAX})
		for line: Variant in page.get("lines", []):
			lines.append(String(line))
		var next := int(page.get("next_cursor", cursor))
		if next >= int(page.get("next", next)) or next == cursor:
			return lines
		cursor = next
	return lines


func get_recent_projects() -> PackedStringArray:
	return PackedStringArray(state(["preferences"]).get("preferences", {}).get("recent_projects", []))


func has_unsaved_prompt() -> bool:
	return bool(state(["dialogs"]).get("dialogs", {}).get("unsaved_prompt", {}).get("open", false))


## The unsaved-changes prompt answered: 0 save, 1 discard, 2 cancel.
func resolve_unsaved(choice: int) -> void:
	if choice < 0 or choice > 2:
		return
	request({"kind": "resolve_unsaved", "choice": ["save", "discard", "cancel"][choice]})


## The Problems query's answer as JSON text (every row, page after page, when the query names no
## limit), or {"error": why}.
func get_problems_json(text := "{}") -> String:
	var args: Variant = JSON.parse_string(text)
	if not (args is Dictionary):
		return JSON.stringify({"error": "the query is not a JSON object"})
	var answer := query("problems", args)
	if answer.has("error") or (args as Dictionary).has("limit"):
		return JSON.stringify(answer)
	var rows: Array = answer.get("problems", [])
	var groups: Array = answer.get("groups", [])
	while answer.get("next_offset") != null:
		var page_args: Dictionary = (args as Dictionary).duplicate()
		page_args["offset"] = int(answer["next_offset"])
		page_args["limit"] = PAGE_MAX
		answer = query("problems", page_args)
		rows.append_array(answer.get("problems", []))
		for group: Variant in answer.get("groups", []):
			if groups.is_empty() or String(groups[-1].get("key", "")) != String(group.get("key", "")):
				groups.append(group)
	var whole := answer.duplicate()
	whole["problems"] = rows
	whole["offset"] = 0
	whole["next_offset"] = null
	if whole.has("groups"):
		whole["groups"] = groups
	return JSON.stringify(whole)


## The running operation, the last one and the last build as JSON text (the operation query).
func get_operation_json() -> String:
	return JSON.stringify(query("operation"))


## A menu's screen as the render check compiled it, every widget and note, as JSON text.
func get_menu_render_json(path: String, screen: int) -> String:
	var answer := query("menu_render", {"path": path, "screen": screen, "limit": PAGE_MAX})
	var widgets: Array = answer.get("widgets", [])
	var notes: Array = answer.get("notes", [])
	while answer.get("next_offset") != null:
		answer = query("menu_render", {"path": path, "screen": screen, "offset": int(answer["next_offset"]),
				"limit": PAGE_MAX})
		widgets.append_array(answer.get("widgets", []))
		notes.append_array(answer.get("notes", []))
	answer["widgets"] = widgets
	answer["notes"] = notes
	return JSON.stringify(answer)


func _project() -> Dictionary:
	return state(["project"]).get("project", {})


func _run() -> Dictionary:
	return state(["run"]).get("run", {})


func _open_at(dir: String) -> bool:
	var project := _project()
	return bool(project.get("open", false)) and String(project.get("root", "")) == dir.replace("\\", "/")


# --- the documents --------------------------------------------------------------------------------

## True when the file was made (or was there) and, for a kind the editor edits, opened; a kind it
## makes but does not edit (a font) is made, not opened.
func create_file(path: String) -> bool:
	return done({"kind": "create_file", "path": path})


func open_document(path: String) -> bool:
	request({"kind": "open_document", "path": path})
	return not query("document", {"path": path, "limit": 1}).has("error")


func get_row_count() -> int:
	return int(_active().get("count", 0))


func get_row_id(index: int) -> int:
	var row := _row(index)
	return int(row.get("id", 0))


func get_row_name(index: int) -> String:
	return String(_row(index).get("name", ""))


## A new record of the document type's kind name ("weapon", "action", "window"): a row when
## `parent` is 0, else inside the record (or row) `parent`, at `position` among that owner's
## records of the kind (-1 the end). The new identity, or 0.
func add_record(kind: String, parent := 0, position := -1) -> int:
	if parent < 0:
		return 0
	var edit := {"op": "add", "kind": kind}
	if parent > 0:
		edit["parent"] = parent
	if position >= 0:
		edit["position"] = position
	var came := _edit([edit])
	var added: Array = came.get("added", [])
	return int(added[0]) if bool(came.get("done", false)) and not added.is_empty() else 0


func remove_record(id: int) -> bool:
	return bool(_edit([{"op": "remove", "id": id}]).get("done", false))


## A field set to a number, a text or a bool (0 or 1).
func set_field(id: int, field: String, value: Variant) -> bool:
	match typeof(value):
		TYPE_INT, TYPE_FLOAT, TYPE_STRING, TYPE_BOOL:
			pass
		_:
			return false
	return bool(_edit([{"op": "set", "id": id, "field": field, "value": value}]).get("done", false))


## An optional field left out of the file (its value kept for when it is written again).
func clear_field(id: int, field: String) -> bool:
	return bool(_edit([{"op": "clear", "id": id, "field": field}]).get("done", false))


## An optional field the file leaves out, written again with the value it reads.
func write_field(id: int, field: String) -> bool:
	return bool(_edit([{"op": "write", "id": id, "field": field}]).get("done", false))


## A field's value as a Variant of its type (an integer, a real, a text); null for an optional
## field the file leaves out, or no such record or field.
func get_field(id: int, field: String) -> Variant:
	var record := query("record", {"id": id})
	for entry: Variant in record.get("fields", []):
		if String(entry.get("id", "")) != field:
			continue
		if bool(entry.get("optional", false)) and not bool(entry.get("present", true)):
			return null
		var value: Variant = entry.get("value")
		match String(entry.get("type", "")):
			"real":
				return float(value)
			"text":
				return String(value)
			_:
				return int(value)
	return null


## Every document with unsaved edits written; true when none is left unsaved.
func save_documents() -> bool:
	request({"kind": "save_all"})
	for document: Variant in state(["documents"]).get("documents", {}).get("open", []):
		if bool(document.get("dirty", false)):
			return false
	return true


func undo() -> void:
	request({"kind": "undo"})


func redo() -> void:
	request({"kind": "redo"})


func is_document_dirty() -> bool:
	return bool(query("document", {"limit": 1}).get("dirty", false))


## The records a row or a record holds (every collection, or the one of kind name `kind`).
func get_child_records(id: int, kind := "") -> PackedInt64Array:
	var ids := PackedInt64Array()
	for collection: Variant in query("record", {"id": id}).get("collections", []):
		if not kind.is_empty() and String(collection.get("kind_name", "")) != kind:
			continue
		for record: Variant in collection.get("records", []):
			ids.append(int(record.get("id", 0)))
	return ids


## The record holding a record (0 for a row).
func get_record_owner(id: int) -> int:
	var owner: Variant = query("record", {"id": id}).get("owner")
	if not (owner is Dictionary):
		return 0
	return int(owner.get("child", 0)) if int(owner.get("child", 0)) != 0 else int(owner.get("row", 0))


func get_record_name(id: int) -> String:
	return String(query("record", {"id": id}).get("name", ""))


## The record defining `symbol` in `scope` ("" any): its identity, 0 for none.
func find_record(symbol: String, scope := "") -> int:
	var args := {"symbol": symbol}
	if not scope.is_empty():
		args["scope"] = scope
	return int(query("record", args).get("id", 0))


## The record copied right after itself.
func duplicate_record(id: int) -> bool:
	return bool(_edit([{"op": "duplicate", "id": id}]).get("done", false))


## The record moved to `position` among its siblings or, with `parent`, among the records of
## another owner in its row.
func move_record(id: int, position: int, parent := 0) -> bool:
	if position < 0 or parent < 0:
		return false
	var edit := {"op": "move", "id": id, "position": position}
	if parent > 0:
		edit["parent"] = parent
	return bool(_edit([edit]).get("done", false))


## A record selected ("replace", "add" to the selection, "toggle" in or out of it).
func select_record(id: int, mode := "replace") -> bool:
	var record := query("record", {"id": id})
	if record.has("error") or not (mode in ["replace", "add", "toggle"]):
		return false
	var address := {"row": record["row"], "kind": record["kind"], "child": record["child"]}
	request({"kind": "select_record", "path": String(record.get("document", "")), "address": address, "mode": mode})
	return true


## The selected records' identities (the primary among them).
func get_selected_records() -> PackedInt64Array:
	var ids := PackedInt64Array()
	for address: Variant in state(["selection"]).get("selection", {}).get("records", []):
		ids.append(int(address["child"]) if int(address.get("child", 0)) != 0 else int(address.get("row", 0)))
	return ids


func copy_records() -> bool:
	return _active_done({"kind": "copy"})


func cut_records() -> bool:
	return _active_done({"kind": "cut"})


## The clipboard pasted into `parent` at `position` (0 and -1: after the selection).
func paste_records(parent := 0, position := -1) -> bool:
	if parent < 0:
		return false
	var fields := {"kind": "paste"}
	if parent > 0 or position >= 0:
		var at := {"parent": parent}
		if position >= 0:
			at["position"] = position
		fields["paste_at"] = at
	return _active_done(fields)


## The coalesced edit group (typing) or the gesture (a drag) of the active document ends.
func end_edit() -> void:
	request({"kind": "end_edit"})


## The active document's first page of rows.
func _active() -> Dictionary:
	return query("document", {"limit": PAGE_MAX})


func _row(index: int) -> Dictionary:
	if index < 0:
		return {}
	var page := query("document", {"offset": index, "limit": 1})
	var rows: Array = page.get("rows", [])
	return rows[0] if not rows.is_empty() else {}


## An edit_record batch on the active document: its outcome ({} when no document is open or the
## batch did not read).
func _edit(edits: Array) -> Dictionary:
	var active := String(state(["documents"]).get("documents", {}).get("active", ""))
	if active.is_empty():
		return {}
	return outcome({"kind": "edit_record", "path": active, "edits": edits})


## A request on the active document, done or not (false when no document is open).
func _active_done(fields: Dictionary) -> bool:
	var active := String(state(["documents"]).get("documents", {}).get("active", ""))
	if active.is_empty():
		return false
	fields["path"] = active
	return done(fields)
