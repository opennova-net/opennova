class_name EditorMcpCatalog
extends RefCounted

## The editor MCP's tool definitions (docs/mcp.md), generated at startup from the session's own
## tables (ADR 0046 S13 A5): `editor_query catalog` answers every request kind with the fields it
## takes, every request field, every query with its params and every state section, each with its
## doc, and editor_request, editor_query, editor_state and editor_viewport are made from it. What a
## table cannot carry (how a request answers, how a query pages, which viewport op is a read and
## which a request, the build, Play, a picture, the transport's log) is written here, once each.

const SCREENSHOT_TIMEOUT_MS := 60_000
## Play builds first, and editor_build waits for its operation.
const BUILD_TIMEOUT_MS := 300_000
## The most entries a query's page holds when the catalog does not say (its page_max).
const PAGE_MAX := McpJson.MAX_ENTRIES

const PLAY_OPS: Array[String] = ["start", "stop", "state"]
## How a Play runs (the session's PlayMode tokens): the OpenNova runtime, the game install, or Strict Play
## in it. A project's own (apply_project_settings' play_mode, its .opennova/local.json), or one Play's.
const PLAY_MODES: Array[String] = ["runtime", "install", "strict"]
## What the previews of the project's own data draw behind their picture (the session's PreviewBackground
## tokens), the editor's preference: set_preview_background sets it, the preferences section says it.
const PREVIEW_BACKGROUNDS: Array[String] = ["dark", "grey", "light", "checker"]

## editor_viewport's writes (S13 V7): each a request, the members it takes flat beside op and the one
## it needs, and where the tool's `kind` goes (the request's member that names the viewport's kind):
## options and camera a set_viewport of the viewport's state (`device`, its device's size, beside the
## options or the camera; kind the change's), seek a set_viewport of the clock alone, which names no
## document and no kind (the one preview clock every viewport reads), try, click and key a menu's Try
## mode (DI-35: try {on, reset}, the game's click at a point, one key or a text typed), drag, command and
## drop (S14) an edit_in_viewport (kind the drag's, the command's or the drop's). Its reads are the
## viewport query's own op, the tokens the catalog lists for it (viewport_reads).
const VIEWPORT_WRITES := {
	"options": {"kind": "set_viewport", "takes": ["options", "device"], "needs": "options", "kind_in": "viewport"},
	"camera": {"kind": "set_viewport", "takes": ["camera", "device"], "needs": "camera", "kind_in": "viewport"},
	"try": {"kind": "set_viewport", "takes": ["try"], "needs": "try", "kind_in": "viewport"},
	"click": {"kind": "set_viewport", "takes": ["click"], "needs": "click", "kind_in": "viewport"},
	"key": {"kind": "set_viewport", "takes": ["key"], "needs": "key", "kind_in": "viewport"},
	"seek": {"kind": "set_viewport", "takes": ["clock"], "needs": "clock", "pathless": true},
	"drag": {"kind": "edit_in_viewport", "takes": ["drag"], "needs": "drag", "kind_in": "drag"},
	"command": {"kind": "edit_in_viewport", "takes": ["command"], "needs": "command", "kind_in": "command"},
	"drop": {"kind": "edit_in_viewport", "takes": ["drop"], "needs": "drop", "kind_in": "drop"},
}

const VIEWPORT_PROSE := (
		"A document's viewport (S13 V7): its picture as the game would draw it, of the kind kind names (menu, "
		+ "model), else the kind it shows in (the Preview's kind that shows it: a menu's screen, a model, a clip "
		+ "or an animation table on its rig's model; else its Main view), headless included. path names the "
		+ "document as editor_query takes it, the active one when left out (refused when it shows in no "
		+ "viewport: a stylesheet feeds the menu's and shows in none). The reads (the viewport query's ops, "
		+ "below) are followed first so they answer the document as it is now (a first read of a document "
		+ "makes its viewport, which can move view_revision); a refusal is a tool error naming the query. op "
		+ "options and camera change its state (a set_viewport: options {...} the kind's options, camera {...} "
		+ "its camera, each with device {width, height}, its device's size, beside it); op try, click and key "
		+ "are a menu's Try mode (DI-35: the picture behaving as the game's menu through the runtime's own driver, "
		+ "a sandbox; try {on, reset?} turns it on from the screen shown or off, reset back to where it started; "
		+ "click {at: [x, y]} the game's click there in design units; key {key, shift?} one key by its name "
		+ "(VK_RETURN, VK_ESCAPE, VK_TAB, ... or one character) or {text} characters typed; the answer's "
		+ "viewport.body.try is where the game's menu is, the screens it went through, what the game would "
		+ "have done (a mission started, a quit) and what the sandbox holds, its items the screen's windows); op seek sets the "
		+ "preview clock every viewport reads (clock {playing, rate, time_ms, ticks}; it names no document and "
		+ "no kind, and takes no path); op drag, command and drop edit through it (an edit_in_viewport: drag {...}, "
		+ "command {...}, drop {...}, a file or a reference released on the picture at a point; a gesture's "
		+ "samples are consecutive drags of one handle on its document, gesture "
		+ "the token the first's answer gave, end false keeping it open; any other request on the document, "
		+ "another gesture, or 10 s with no sample ends it). Each write answers as editor_request answers (ok, "
		+ "served, outcome: done, findings, a drag's gesture; status, view_revision; a request that did not read "
		+ "is a tool error, one refused is ok with an outcome not done) with the viewport's state after it, "
		+ "viewport (none after a seek with nothing to show). Its device takes what changed at the editor's "
		+ "next frame (device.attached, builds, a widget's device_rect) where the Preview window or a canvas "
		+ "draws it; one neither draws holds no device.")

const REQUEST_PROSE := (
		"Raise one typed editor request by kind, the vocabulary the windows use (the request table, "
		+ "engine/editor/session/request_kinds.cpp). Each kind takes its own fields, each field meaning one thing "
		+ "whatever the kind; a field the kind does not take, or one it must carry left out, is refused naming "
		+ "what it takes. path left out names the active document where the kind acts on one. edits is the "
		+ "batch form, one undo step: its forms and ops follow the kinds and its members are its schema's, both "
		+ "the session's batch table (engine/editor/session/record_batch.h, editor_query catalog's batch), a "
		+ "record by its identity (as editor_query document and record give it) or by the label (as) an earlier "
		+ "add or duplicate of the batch gave. "
		+ "The answer: ok (it read), served, and outcome: done (false when it was refused, did not finish, or "
		+ "waits on the unsaved-changes prompt: editor_state's dialogs say what waits, resolve_unsaved answers), "
		+ "unsaved_prompt, operation (the one it started or joined, 0 for none: open_project, new_project, rescan, "
		+ "reimport, the import previews and import_files, the renames, build and play start one; the request "
		+ "returns at once and editor_query operation shows it stepping, done of total, cancellable), findings, "
		+ "added (the records its edits made, in "
		+ "order) and, for an edit_record, made {label: id}; then status and view_revision (the view's clock "
		+ "after it, which editor_state's since takes). wait (this tool's, not the request's): true awaits the "
		+ "operation the request started or joined and the validation after it (the polls step it: no request "
		+ "runs it, an edit's included), then answers with operation (what it came to: id, kind, end done, "
		+ "failed or cancelled, the findings a rename's commit, an import's write or a reimport made, and an "
		+ "import's imported and not_imported files) and the status and view_revision as it left them; wait_ms "
		+ "(300000 when left out) bounds it, past which the answer says timed_out: true, its operation the "
		+ "running operation's state and its validation the validation's. A request that "
		+ "asks open_first is read once before its "
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

## The request fields whose schema says more than their type (edits: the batch table's, edit_schema).
const FIELD_SCHEMAS := {
	"new_name": {"type": ["string", "integer"]},
	"mode": {"type": "string", "enum": ["replace", "add", "toggle"]},
	"choice": {"type": "string", "enum": ["save", "discard", "cancel"]},
	"play_mode": {"type": "string", "enum": PLAY_MODES},
	"preview_background": {"type": "string", "enum": PREVIEW_BACKGROUNDS},
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
			+ "waited on (id, end, findings). With report false, the build result's panel stays closed as it ends "
			+ "(the person's work left as it is).",
			{
				"report": {"type": "boolean", "description": "The build result's panel opens over the editor as "
						+ "the build ends (true when left out)."},
			}, [], true, BUILD_TIMEOUT_MS),
		McpToolDef.make("editor_play",
			"op=start: build, waiting as editor_build does, then run the game on the build (the runtime beside "
			+ "the editor, or this Godot binary in a source run) with its own MCP endpoint, its port allocated as "
			+ "the build lands; the reply is the run section (state, pid, mcp_port: game_state, game_menu and "
			+ "game_probe on that port drive it, mission, exit_code). With mission (op=start), a .bms of the "
			+ "project by its logical name, the game starts in that mission instead of at its menu (one the "
			+ "project does not hold is refused, play.mission.unknown; one that does not load is a "
			+ "play.mission.failed Problems row). With behind (op=start; Windows only), the game's window starts "
			+ "behind every other window, the editor keeping the foreground, until the person brings it forward "
			+ "(the run section's behind). The run directory keeps what the game wrote there in the Plays of "
			+ "the same mode before (its game.cfg, which spares the game install its device dialog, its saves: "
			+ "the run section's kept); with fresh (op=start) it is emptied first, a first run. Play from here "
			+ "(op=start, DI-26): start {at: [x, y, z], yaw?} with mission starts the player at that point of the "
			+ "mission facing yaw (compass degrees); from_here true has the mission's view (mission's, else the "
			+ "active document's) start it on the ground under its camera, or under the picture point at [x, y], "
			+ "facing the way the camera looks. Neither game takes a place on its command line: the build's copy of "
			+ "the mission in the run directory gets the start markers its player deploys at moved there (the run "
			+ "section's start), never the project's file. How it runs is the project's own play_mode (runtime, "
			+ "install or strict: kept in its .opennova/local.json, set by apply_project_settings; runtime for a "
			+ "project never set), or play_mode (op=start) for this Play alone, the project's left as it is; the run "
			+ "section says play_mode (the project's) and ran_mode (the game's). With unsaved documents Play saves "
			+ "them first while save_before_play is on (this Play's, else the project's; on by default). op=stop "
			+ "ends the game and waits; op=state reads the run section.",
			{
				"op": {"type": "string", "enum": PLAY_OPS},
				"mission": {"type": "string", "description": "op=start: the mission the game starts in (04TR.bms)"},
				"behind": {"type": "boolean", "description": "op=start: the game's window behind every other, the "
						+ "editor keeping the foreground (Windows only; false when left out)"},
				"fresh": {"type": "boolean", "description": "op=start: the run directory emptied of what the runs "
						+ "before wrote there (game.cfg, saves) before the game starts (false when left out)"},
				"start": {"type": "object", "description": "op=start, with mission: where the player starts, "
						+ "{at: [x, y, z] mission metres (x east, y north, z up), yaw?: compass degrees}"},
				"from_here": {"type": "boolean", "description": "op=start: Play from here in the mission's view "
						+ "(mission's, else the active document's): the player on the ground under its camera"},
				"at": {"type": "array", "items": {"type": "number"}, "description": "op=start with from_here: the "
						+ "picture point [x, y] (the view's pixels) whose ground the player starts on"},
				"play_mode": {"type": "string", "enum": PLAY_MODES, "description": "op=start: how this Play runs "
						+ "(runtime: the OpenNova runtime; install: the game install; strict: Strict Play in it), for "
						+ "this Play alone; left out, the project's own (runtime unless the project was set otherwise)"},
				"save_before_play": {"type": "boolean", "description": "op=start: this Play writes the unsaved "
						+ "files first (true) or waits on the unsaved-changes prompt (false); left out, the project's own"},
			}, ["op"], true, BUILD_TIMEOUT_MS),
		_viewport_tool(catalog),
		McpToolDef.make("editor_screenshot",
			"Capture the editor window (its ImGui workspace, the Preview window's picture and the Document tab's); "
			+ "a headless editor refuses.",
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


## The viewport query's ops (its op param's enum in the catalog): editor_viewport's reads.
static func viewport_reads(catalog: Dictionary) -> Array[String]:
	var ops: Array[String] = []
	for param: Variant in _query_row(catalog, "viewport").get("params", []):
		if String((param as Dictionary).get("name", "")) == "op":
			for token: Variant in (param as Dictionary).get("enum", []):
				ops.append(String(token))
	return ops


## A query's row in the catalog by name ({} for none).
static func _query_row(catalog: Dictionary, name: String) -> Dictionary:
	for row: Variant in catalog.get("queries", []):
		if String((row as Dictionary).get("name", "")) == name:
			return row as Dictionary
	return {}


## The state's sections, by name.
static func section_names(catalog: Dictionary) -> Array[String]:
	var names: Array[String] = []
	for row: Variant in catalog.get("sections", []):
		names.append(String(row.get("name", "")))
	return names


## One edit of editor_request's `edits`, made from the session's batch table (the catalog's batch:
## engine/editor/session/record_batch.h): every member any form reads, its JSON type, its least
## value or the one string it takes, and what it carries with the forms that read it; op the ops.
static func edit_schema(catalog: Dictionary) -> Dictionary:
	var batch: Dictionary = catalog.get("batch", {})
	var ops: Array[String] = []
	for row: Variant in batch.get("ops", []):
		ops.append(String((row as Dictionary).get("op", "")))
	var properties := {}
	for row: Variant in batch.get("members", []):
		var member: Dictionary = row
		var schema: Dictionary
		match String(member.get("type", "string")):
			"integer":
				schema = {"type": "integer"}
			"id":
				schema = {"type": ["integer", "string"]}
			"boolean":
				schema = {"type": "boolean"}
			"value":
				schema = {"type": ["number", "string", "boolean"]}
			"records":
				schema = {"type": "array", "items": {"type": "object"}}
			_:
				schema = {"type": "string"}
		if member.has("minimum"):
			schema["minimum"] = int(member["minimum"])
		if member.has("only"):
			schema["enum"] = [String(member["only"])]
		var name := String(member.get("name", ""))
		if name == "op":
			schema["enum"] = ops
		var forms: Array[String] = []
		for form: Variant in member.get("forms", []):
			forms.append(String(form))
		schema["description"] = "%s (%s)" % [String(member.get("doc", "")), ", ".join(PackedStringArray(forms))]
		properties[name] = schema
	return {"type": "object", "properties": properties}


## editor_request's `workspace` (set_workspace), made from the session's workspace table (the catalog's
## workspace: engine/editor/session/workspace_parts.h, the MCP gaps lane): a property per part, each an object
## of its members with their JSON types and docs, and focus, the windows it brings forward.
static func workspace_schema(catalog: Dictionary) -> Dictionary:
	var table: Dictionary = catalog.get("workspace", {})
	var properties := {}
	for row: Variant in table.get("parts", []):
		var part: Dictionary = row
		var members := {}
		for item: Variant in part.get("members", []):
			var member: Dictionary = item
			var schema := _json_schema(String(member.get("type", "string")))
			schema["description"] = String(member.get("doc", ""))
			# The longest text its window's field holds: a longer one is refused.
			if member.has("max_length") and String(schema.get("type", "")) == "string":
				schema["maxLength"] = int(member.get("max_length", 0))
			members[String(member.get("name", ""))] = schema
		properties[String(part.get("part", ""))] = {"type": "object", "description": String(part.get("doc", "")),
				"properties": members, "additionalProperties": false}
	properties["focus"] = {"type": "string", "enum": table.get("focus", []),
			"description": "A window brought forward (a focus_window view event), as a click on its tab brings it."}
	return {"type": "object", "properties": properties, "additionalProperties": false}


static func _json_schema(type: String) -> Dictionary:
	match type:
		"integer":
			return {"type": "integer", "minimum": 0}
		"number":
			return {"type": "number"}
		"boolean":
			return {"type": "boolean"}
		"string[]":
			return {"type": "array", "items": {"type": "string"}}
		"integer[]":
			return {"type": "array", "items": {"type": "integer", "minimum": 0}}
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
	# The batch form's forms and ops, from the session's batch table.
	var batch: Dictionary = catalog.get("batch", {})
	lines.append("The edits' forms:")
	for row: Variant in batch.get("forms", []):
		var form: Dictionary = row
		var ops: Array[String] = []
		for op: Variant in batch.get("ops", []):
			if String((op as Dictionary).get("form", "")) == String(form.get("form", "")):
				ops.append("%s: %s" % [String(op.get("op", "")), String(op.get("doc", ""))])
		lines.append("%s: %s%s" % [String(form.get("form", "")), String(form.get("doc", "")),
				(" Its ops: " + " ".join(PackedStringArray(ops))) if not ops.is_empty() else ""])
	var properties := {"kind": {"type": "string", "enum": served},
			"wait": {"type": "boolean", "description": "Await the operation the request starts or joins, and the "
					+ "validation after it, before answering (this tool's, not the request's)."},
			"wait_ms": {"type": "integer", "minimum": 0, "description": "How long wait awaits (300000 when left "
					+ "out); past it the answer says timed_out."}}
	for field: Variant in catalog.get("fields", []):
		var name := String(field.get("field", ""))
		var schema: Dictionary
		if name == "edits":
			schema = {"type": "array", "items": edit_schema(catalog)}
		elif name == "workspace":
			schema = workspace_schema(catalog)
		else:
			schema = FIELD_SCHEMAS.get(name, _json_schema(String(field.get("type", "string")))).duplicate(true)
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


## editor_viewport (S13 V7), made from the catalog: the viewport query's row (its doc, its op's tokens
## the reads, and its params but op as the tool's own, flat beside op, a param's choices its enum) and
## the two requests its writes raise (set_viewport's and edit_in_viewport's docs, and the docs of the
## fields they carry: viewport, drag, command).
static func _viewport_tool(catalog: Dictionary) -> McpToolDef:
	var lines: Array[String] = [VIEWPORT_PROSE]
	var ops: Array[String] = viewport_reads(catalog)
	for op: String in VIEWPORT_WRITES:
		ops.append(op)
	var properties := {"op": {"type": "string", "enum": ops}}
	var query := _query_row(catalog, "viewport")
	if not query.is_empty():
		lines.append("The reads: " + String(query.get("doc", "")))
		for param: Variant in query.get("params", []):
			var key := String(param.get("name", ""))
			if key == "op":
				continue
			var schema := _json_schema(String(param.get("type", "string")))
			if key == "limit":
				schema["minimum"] = 1
				schema["maximum"] = int(catalog.get("page_max", PAGE_MAX))
			if (param as Dictionary).has("enum"):
				schema["enum"] = (param as Dictionary)["enum"]
			schema["description"] = String(param.get("doc", ""))
			properties[key] = schema
	for row: Variant in catalog.get("requests", []):
		var kind := String(row.get("kind", ""))
		if kind == "set_viewport" or kind == "edit_in_viewport":
			lines.append("%s: %s" % [kind, String(row.get("doc", ""))])
	var docs := {}
	for field: Variant in catalog.get("fields", []):
		docs[String(field.get("field", ""))] = String(field.get("doc", ""))
	var change := String(docs.get("viewport", ""))
	for member: String in ["options", "camera", "clock", "device", "try", "click", "key"]:
		properties[member] = {"type": "object", "description": "op %s: set_viewport's %s, as its viewport field takes it: %s"
				% ["seek" if member == "clock" else ("options or camera" if member == "device" else member), member, change]}
	for member: String in ["drag", "command", "drop"]:
		properties[member] = {"type": "object", "description": "op %s: edit_in_viewport's: %s" % [member, docs.get(member, "")]}
	return McpToolDef.make("editor_viewport", " ".join(PackedStringArray(lines)), properties, ["op"], true)
