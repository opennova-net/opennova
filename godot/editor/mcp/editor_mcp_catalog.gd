class_name EditorMcpCatalog
extends RefCounted

## The editor MCP's tool definitions (docs/mcp.md), generated at startup from the session's own
## tables (ADR 0046 S13 A5): `editor_query catalog` answers every request kind with the fields it
## takes, every request field, every query with its params and every state section, each with its
## doc, and editor_request, editor_query and editor_state are made from it. What a table cannot
## carry (how a request answers, how a query pages, the build, Play, the previews, a picture, the
## transport's log) is written here, once each.

const SCREENSHOT_TIMEOUT_MS := 60_000
## Play builds first, and editor_build waits for its operation.
const BUILD_TIMEOUT_MS := 300_000
## The preview tools' pages (until S13 V7's viewport tool): the result sanitizer caps a list at
## McpJson.MAX_ENTRIES.
const PAGE_MAX := McpJson.MAX_ENTRIES
const PAGE_DEFAULT := 100

const PLAY_OPS: Array[String] = ["start", "stop", "state"]

const MENU_PREVIEW_OPS: Array[String] = ["state", "rects", "hit", "options", "drag", "nudge", "arrange", "notes"]
## editor_menu_preview op=arrange (editor/preview/menu_arrange.h's tokens).
const ARRANGE_OPS: Array[String] = ["align_left", "align_right", "align_top", "align_bottom",
		"align_horizontal_centers", "align_vertical_centers", "distribute_horizontally", "distribute_vertically",
		"bring_to_front", "bring_forward", "send_backward", "send_to_back"]
const MENU_PREVIEW_STATES: Array[String] = ["normal", "mouseover", "selected", "disabled"]
const MENU_PREVIEW_HANDLES: Array[String] = ["move", "left", "right", "top", "bottom", "top_left", "top_right",
		"bottom_left", "bottom_right"]
const MODEL_PREVIEW_OPS: Array[String] = ["state", "options", "camera", "hit", "drag"]
const MODEL_PREVIEW_HANDLES: Array[String] = ["place", "axis"]

## One edit of editor_request's `edits`, the batch form (engine/editor/session/record_batch.h):
## records by identity or by the label an earlier add or duplicate of the batch gave with `as`;
## over a text document (S13 D9) a span of its text replaced, op apply with payload text.span.
const EDIT_SCHEMA := {
	"type": "object",
	"properties": {
		"op": {"type": "string",
				"enum": ["set", "clear", "write", "add", "duplicate", "remove", "move", "set_file_value", "replace_list",
						"apply"]},
		"id": {"type": ["integer", "string"], "description": "the record: its identity, or a label an earlier edit gave"},
		"parent": {"type": ["integer", "string"], "description": "an add's owner (a row's identity for a record in it; none adds a row), a move's destination"},
		"kind": {"type": "string", "description": "an add's record kind token (window, action, sound, items.item, ...)"},
		"field": {"type": "string"},
		"value": {},
		"position": {"type": "integer", "minimum": 0},
		"as": {"type": "string", "description": "the label an add or a duplicate gives the record it makes"},
		"coalesce": {"type": "boolean"},
		"gesture": {"type": "integer", "minimum": 0},
		"list": {"type": "string", "description": "a replace_list's collection kind token"},
		"records": {"type": "array", "items": {"type": "object"}, "description": "a replace_list's records, each {field: value, ...}"},
		"payload": {"type": "string", "enum": ["text.span"], "description": "an apply's change: a text document's span replaced"},
		"line": {"type": "integer", "minimum": 1, "description": "an apply's span: its line, from 1"},
		"column": {"type": "integer", "minimum": 1, "description": "an apply's span: its column, from 1"},
		"length": {"type": "integer", "minimum": 0, "description": "an apply's span: the characters it replaces (a line end counts its own), 0 by default"},
		"text": {"type": "string", "description": "an apply's span: what takes its place (Windows-1252 characters)"},
	},
}

const REQUEST_PROSE := (
		"Raise one typed editor request by kind, the vocabulary the windows use (the request table, "
		+ "engine/editor/session/request_kinds.cpp). Each kind takes its own fields, each field meaning one thing "
		+ "whatever the kind; a field the kind does not take, or one it must carry left out, is refused naming "
		+ "what it takes. path left out names the active document where the kind acts on one. edits is the "
		+ "batch form: [{op, id, parent, kind, field, value, position, as, coalesce, gesture}] over any rows, one "
		+ "undo step, a record by its identity (as editor_query document and record give it) or by the label "
		+ "(as) an earlier add or duplicate of the batch gave, an add's kind by its token (op replace_list "
		+ "{id, list, records}: the list replaced by these records); revert_to_saved's edits are [{id, field}]; "
		+ "over a text document (a script, a music script, credits, a shader, a configuration) the edits are its "
		+ "spans replaced, [{op: apply, payload: text.span, line, column, length, text}], each against the text as "
		+ "the ones before left it (editor_query document pages its lines). "
		+ "The answer: ok (it read), served, and outcome: done (false when it was refused, did not finish, or "
		+ "waits on the unsaved-changes prompt: editor_state's dialogs say what waits, resolve_unsaved answers), "
		+ "unsaved_prompt, operation (the one it started or joined, 0 for none; build and play return at once "
		+ "and editor_query operation shows it stepping), findings, added (the records its edits made, in "
		+ "order) and, for an edit_record, made {label: id}; then status and view_revision (the view's clock "
		+ "after it, which editor_state's since takes). A request that asks open_first is read once before its "
		+ "document opens: one refused as it is read opens nothing. While an operation runs, a request that conflicts "
		+ "with what it reads or writes is refused (operation.busy); the pickers are refused, pass paths "
		+ "instead. A problem's fixes (editor_query problems) are requests of these kinds, passed back as they "
		+ "are. The kinds:")

const QUERY_PROSE := (
		"Ask the editor what it holds, by query name (the query table, engine/editor/session/editor_queries.cpp): "
		+ "the params the query takes, flat beside `query`; a param it does not take, or one it needs left "
		+ "out, is refused naming what it takes. Every answer carries view_revision, the view's clock value at "
		+ "which what it reads last moved (a document's or a menu's own revision is its own): a later answer "
		+ "past it has moved, and editor_state's since takes it back. A path left out names the active "
		+ "document (a menu read's too: refused when the active document is no menu). A list is served a page "
		+ "at a time: offset (or cursor, for the output lines and the events, which move on while they are "
		+ "read: a client more than 2000 lines or 64 events behind finds the cursor larger than it asked) and "
		+ "limit (1 to 200, 100 by default); count is the list's whole length, next_offset the next page's "
		+ "(null at the end; where one page covers several lists, it runs to the end of the longest), "
		+ "next_cursor the next cursor. Offset pages are gapless while the list stays as it was. The queries:")

const STATE_PROSE := (
		"Read the editor's state by section: view_revision (the view's clock) and revisions (each "
		+ "concern's stamp, the clock value at which it last moved), then each section asked for (every one "
		+ "when sections is left out); since, a view_revision any earlier answer carried (a query's or a "
		+ "request's too), leaves out the sections none of whose concerns moved since (0 or left out: every "
		+ "section; one past the clock is refused). The lists are editor_query's (files, problems, output, "
		+ "events, import_preview). The sections:")

## The request fields whose schema says more than their type.
const FIELD_SCHEMAS := {
	"new_name": {"type": ["string", "integer"]},
	"edits": {"type": "array", "items": EDIT_SCHEMA},
	"mode": {"type": "string", "enum": ["replace", "add", "toggle"]},
	"choice": {"type": "string", "enum": ["save", "discard", "cancel"]},
}


## The tool list, from the session's catalog.
static func definitions(app: Node) -> Array[McpToolDef]:
	var catalog := catalog_of(app)
	return [
		_state_tool(catalog),
		_request_tool(catalog),
		_query_tool(catalog),
		McpToolDef.make("editor_build",
			"Pack the project to an immutable build directory and wait for it: build raised as the windows "
			+ "raise it, then frames awaited until its operation ends (editor_state and editor_query answer the "
			+ "while; a build running already is joined); refused while required files are missing; with "
			+ "unsaved documents it waits on the unsaved-changes prompt (editor_request resolve_unsaved save "
			+ "writes them and builds). Returns the build (ok, dir, archives, diagnostics) with the operation it "
			+ "waited on (id, end, findings).",
			{}, [], true, BUILD_TIMEOUT_MS),
		McpToolDef.make("editor_play",
			"op=start: build, waiting as editor_build does, then run the game on the build (the runtime beside "
			+ "the editor, or this Godot binary in a source run) with its own MCP endpoint, its port allocated as "
			+ "the build lands; the reply is the run section (state, pid, mcp_port: game_state, game_menu and "
			+ "game_probe on that port drive it, exit_code). op=stop ends the game and waits; op=state reads the "
			+ "run section.",
			{"op": {"type": "string", "enum": PLAY_OPS}}, ["op"], true, BUILD_TIMEOUT_MS),
		_menu_preview_tool(),
		_model_preview_tool(),
		McpToolDef.make("editor_screenshot",
			"Capture the editor window (its ImGui workspace, the menu preview and the model preview); a headless "
			+ "editor refuses.",
			{
				"max_dim": {"type": "integer", "minimum": 64, "maximum": 4096, "default": 1280},
				"format": {"type": "string", "enum": ["webp", "png"], "default": "webp"},
				"quality": {"type": "number", "minimum": 0.1, "maximum": 1.0, "default": 0.8},
			}, [], true, SCREENSHOT_TIMEOUT_MS),
		McpToolDef.make("editor_logs",
			"The editor MCP's own log (sources server and script); the editor's output lines (the build log, the "
			+ "running game's log) are editor_query output.",
			{
				"cursor": {"type": "integer", "minimum": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": 2000, "default": 200},
			}, [], false),
	]


## The session's catalog: {requests, fields, queries, sections, concerns, finding_codes, page_max}.
static func catalog_of(app: Node) -> Dictionary:
	var answer: Variant = JSON.parse_string(String(app.call("query_json", "catalog", "{}")))
	return answer if answer is Dictionary else {}


## The request kinds editor_request serves, by token: every row but those a person answers (the
## pickers, whose paths a client passes instead).
static func request_kinds(catalog: Dictionary) -> Array[String]:
	var kinds: Array[String] = []
	for row: Variant in catalog.get("requests", []):
		if String(row.get("served_by", "")) != "person":
			kinds.append(String(row.get("kind", "")))
	return kinds


## The queries editor_query answers, by name.
static func query_names(catalog: Dictionary) -> Array[String]:
	var names: Array[String] = []
	for row: Variant in catalog.get("queries", []):
		names.append(String(row.get("name", "")))
	return names


## The state's sections, by name.
static func section_names(catalog: Dictionary) -> Array[String]:
	var names: Array[String] = []
	for row: Variant in catalog.get("sections", []):
		names.append(String(row.get("name", "")))
	return names


static func _json_schema(type: String) -> Dictionary:
	match type:
		"integer":
			return {"type": "integer", "minimum": 0}
		"boolean":
			return {"type": "boolean"}
		"string[]":
			return {"type": "array", "items": {"type": "string"}}
		"object":
			return {"type": "object"}
		"object[]":
			return {"type": "array", "items": {"type": "object"}}
	return {"type": "string"}


static func _state_tool(catalog: Dictionary) -> McpToolDef:
	var lines: Array[String] = [STATE_PROSE]
	for row: Variant in catalog.get("sections", []):
		lines.append("%s: %s" % [String(row.get("name", "")), String(row.get("doc", ""))])
	return McpToolDef.make("editor_state", " ".join(PackedStringArray(lines)), {
		"sections": {"type": "array", "items": {"type": "string", "enum": section_names(catalog)}},
		"since": {"type": "integer", "minimum": 0},
	}, [], false)


static func _request_tool(catalog: Dictionary) -> McpToolDef:
	var lines: Array[String] = [REQUEST_PROSE]
	var served := request_kinds(catalog)
	for row: Variant in catalog.get("requests", []):
		var kind := String(row.get("kind", ""))
		if not served.has(kind):
			continue
		var needs: Array = row.get("needs", [])
		var takes: Array[String] = []
		for field: Variant in row.get("takes", []):
			takes.append(String(field) + ("" if needs.has(field) else "?"))
		var fields := (" {%s}" % ", ".join(PackedStringArray(takes))) if not takes.is_empty() else ""
		lines.append("%s%s: %s" % [kind, fields, String(row.get("doc", ""))])
	var properties := {"kind": {"type": "string", "enum": served}}
	for field: Variant in catalog.get("fields", []):
		var name := String(field.get("field", ""))
		var schema: Dictionary = FIELD_SCHEMAS.get(name, _json_schema(String(field.get("type", "string")))).duplicate(true)
		schema["description"] = String(field.get("doc", ""))
		properties[name] = schema
	return McpToolDef.make("editor_request", " ".join(PackedStringArray(lines)), properties, ["kind"], true, BUILD_TIMEOUT_MS)


static func _query_tool(catalog: Dictionary) -> McpToolDef:
	var lines: Array[String] = [QUERY_PROSE]
	var properties := {"query": {"type": "string", "enum": query_names(catalog)}}
	# A param several queries take: one schema, its doc per query where they differ.
	var docs := {}
	for row: Variant in catalog.get("queries", []):
		var name := String(row.get("name", ""))
		var params: Array[String] = []
		for param: Variant in row.get("params", []):
			var key := String(param.get("name", ""))
			params.append(key + ("" if bool(param.get("required", false)) else "?"))
			if not properties.has(key):
				var schema := _json_schema(String(param.get("type", "string")))
				if key == "limit":
					schema["minimum"] = 1
					schema["maximum"] = int(catalog.get("page_max", PAGE_MAX))
				properties[key] = schema
				docs[key] = {}
			var doc := String(param.get("doc", ""))
			if not docs[key].has(doc):
				docs[key][doc] = []
			docs[key][doc].append(name)
		var list := String(row.get("list", ""))
		lines.append("%s%s: %s%s" % [name, (" {%s}" % ", ".join(PackedStringArray(params))) if not params.is_empty() else "",
				String(row.get("doc", "")), (" Pages %s." % list) if not list.is_empty() else ""])
	for key: String in docs:
		var by_doc: Dictionary = docs[key]
		if by_doc.size() == 1:
			properties[key]["description"] = by_doc.keys()[0]
			continue
		var parts: Array[String] = []
		for doc: String in by_doc:
			parts.append("%s: %s" % [", ".join(PackedStringArray(by_doc[doc])), doc])
		properties[key]["description"] = " ".join(PackedStringArray(parts))
	return McpToolDef.make("editor_query", " ".join(PackedStringArray(lines)), properties, ["query"], false)


static func _menu_preview_tool() -> McpToolDef:
	return McpToolDef.make("editor_menu_preview",
		"The menu preview, headless included (a device tool until S13 V7's viewport tool): the previewed "
		+ "screen (the last menu screen selected) as the game would draw it were the menu saved now, the open "
		+ "stylesheets and string tables standing in for their files. op=state: status (no_project, no_menu, "
		+ "no_screen, unserializable, screen_missing, ready) and its message, the menu path and screen, the "
		+ "revision shown and whether it is current, the device size, the options, the files it names that the "
		+ "project lacks (missing) or that did not load (unreadable), widget_count and note_count; rects "
		+ "{offset?, limit?}: a page of the widgets as the runtime placed them (index, id, name, type, shown, "
		+ "disabled, rect and local [left, top, right, bottom] in 800x600 design units, text, font, "
		+ "text_color); hit {x, y}: the widget the game's hit test finds at a design point (index -1 for "
		+ "none); options {width?, height?, show_hidden?, force_id?, force_state?, checked?, popup_open?, "
		+ "focus?}: how it draws, then the state; drag {id, handle, dx, dy, snap?=true}: a window of the "
		+ "previewed screen moved or resized by a handle, the moved edges snapped to the grid of 8, one undo "
		+ "step, then the state; nudge {id, dx, dy}: a move with no snap; arrange {ids, arrange}: windows "
		+ "aligned, distributed or reordered, one undo step, then the state; notes {offset?, limit?}: a page of "
		+ "the frame compiler's notes on the previewed screen. A screen as the render check compiled it is "
		+ "editor_query menu_render.",
		{
			"op": {"type": "string", "enum": MENU_PREVIEW_OPS},
			"x": {"type": "number"},
			"y": {"type": "number"},
			"width": {"type": "integer", "minimum": 1, "maximum": 8192},
			"height": {"type": "integer", "minimum": 1, "maximum": 8192},
			"show_hidden": {"type": "boolean"},
			"force_id": {"type": "integer", "minimum": 0},
			"force_state": {"type": "string", "enum": MENU_PREVIEW_STATES},
			"checked": {"type": "boolean"},
			"popup_open": {"type": "boolean"},
			"focus": {"type": "boolean"},
			"id": {"type": "integer", "minimum": 1},
			"handle": {"type": "string", "enum": MENU_PREVIEW_HANDLES},
			"dx": {"type": "integer"},
			"dy": {"type": "integer"},
			"snap": {"type": "boolean", "default": true},
			"ids": {"type": "array", "items": {"type": "integer", "minimum": 1}},
			"arrange": {"type": "string", "enum": ARRANGE_OPS},
			"offset": {"type": "integer", "minimum": 0, "default": 0},
			"limit": {"type": "integer", "minimum": 1, "maximum": PAGE_MAX, "default": PAGE_DEFAULT},
		}, ["op"])


static func _model_preview_tool() -> McpToolDef:
	return McpToolDef.make("editor_model_preview",
		"The model preview, headless included (a device tool until S13 V7's viewport tool): the previewed "
		+ "model (the last model, clip or animation table made active) as the game would draw it were it saved "
		+ "now, through the runtime's own renderer. op=state: status (no_project, no_model, unserializable, "
		+ "unreadable, ready) and its message, the path, the revision shown and whether it is current, builds, "
		+ "the device size, the options, lod {shown, auto, count, projected_px, thresholds}, camera {target, "
		+ "yaw, pitch, distance, fov}, sphere, registers, the overlays (user points, lights, pivots: each with "
		+ "its record id, position and device pixel), the clock and the animation; options {lod?, ctrl?, "
		+ "playing?, overlays?, time_ms?, rig_model?, clip_ticks?}: how it draws, then the state; camera {yaw?, "
		+ "pitch?, distance?, target?, frame?, width?, height?}: then the state; hit {x, y}: the marker at a "
		+ "device point (index -1 for none); drag {id, handle?=place|axis, x, y, snap?=0}: a user point or a "
		+ "light moved, or its axis turned, to the point under device pixel x, y, one undo step, then the "
		+ "state.",
		{
			"op": {"type": "string", "enum": MODEL_PREVIEW_OPS},
			"lod": {"description": "a level (an integer from 0) or \"auto\""},
			"ctrl": {"type": "object"},
			"playing": {"type": "boolean"},
			"overlays": {"type": "object"},
			"time_ms": {"type": "integer", "minimum": 0},
			"rig_model": {"type": "string"},
			"clip_ticks": {"type": "integer", "minimum": 0},
			"x": {"type": "number"},
			"y": {"type": "number"},
			"id": {"type": "integer", "minimum": 1},
			"handle": {"type": "string", "enum": MODEL_PREVIEW_HANDLES, "default": "place"},
			"snap": {"type": "number", "minimum": 0, "default": 0},
			"yaw": {"type": "number"},
			"pitch": {"type": "number"},
			"distance": {"type": "number"},
			"target": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
			"frame": {"type": "boolean"},
			"width": {"type": "integer", "minimum": 1, "maximum": 8192},
			"height": {"type": "integer", "minimum": 1, "maximum": 8192},
		}, ["op"])
