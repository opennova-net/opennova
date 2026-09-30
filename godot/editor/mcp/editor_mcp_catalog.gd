class_name EditorMcpCatalog
extends RefCounted

## The editor MCP's tool definitions (docs/mcp.md). Twelve tools over the editor's
## typed seam: the state, the generic request, the documents, the problems, the
## build, Play, the asset graph, the menu preview, the model preview, the menu tools, a
## screenshot and the transport's log.

const SCREENSHOT_TIMEOUT_MS := 60_000
## A build packs the whole project inside one call; Play builds first.
const BUILD_TIMEOUT_MS := 300_000
## The list pages (rows, problems, output lines): the result sanitizer caps a list
## at McpJson.MAX_ENTRIES, so a retail catalog's rows and a scan's findings page.
const PAGE_MAX := McpJson.MAX_ENTRIES
const PAGE_DEFAULT := 100

const DOCUMENT_OPS: Array[String] = [
	"list", "open", "create", "close", "reload", "rows", "record", "choices", "targets", "find", "search",
	"set", "clear", "write", "revert", "add", "duplicate", "remove", "move", "select", "copy", "cut",
	"paste",
	"end_edit", "save", "save_all", "undo", "redo",
]

## The edit shape editor_request takes (edit, and each of edits).
const EDIT_SCHEMA := {
	"type": "object",
	"properties": {
		"operation": {
			"type": "string",
			"enum": ["set", "clear", "write", "add", "duplicate", "remove", "move", "paste", "set_file_value"],
		},
		"row": {"type": "integer", "minimum": 0},
		"kind": {"type": "integer"},
		"child": {"type": "integer", "minimum": 0},
		"parent": {"type": "integer", "minimum": 0},
		"field": {"type": "string"},
		"value": {},
		"position": {"type": "integer", "minimum": 0},
		"coalesce": {"type": "boolean"},
		"gesture": {"type": "integer", "minimum": 0},
	},
}

const PLAY_OPS: Array[String] = ["start", "stop", "state"]

const GRAPH_OPS: Array[String] = ["references", "referrers", "usages", "missing", "symbols", "search", "rename", "rename_symbol",
		"assign"]
## The editor_graph ops that answer a list, a page of it (offset, limit; count the total).
const PAGED_GRAPH_OPS: Array[String] = ["references", "referrers", "usages", "missing", "symbols", "search"]

const MENU_PREVIEW_OPS: Array[String] = ["state", "rects", "hit", "options", "drag", "nudge", "arrange", "notes", "render"]
## editor_menu_preview op=arrange (editor/preview/menu_arrange.h's tokens).
const ARRANGE_OPS: Array[String] = ["align_left", "align_right", "align_top", "align_bottom",
		"align_horizontal_centers", "align_vertical_centers", "distribute_horizontally", "distribute_vertically",
		"bring_to_front", "bring_forward", "send_backward", "send_to_back"]
const MENU_PREVIEW_STATES: Array[String] = ["normal", "mouseover", "selected", "disabled"]
## editor_model_preview ops.
const MODEL_PREVIEW_OPS: Array[String] = ["state", "options", "camera", "hit", "drag"]
## editor_model_preview op=drag handles.
const MODEL_PREVIEW_HANDLES: Array[String] = ["place", "axis"]
## editor_menu: a menu's tree, a batch by record id and label, a list replaced, its findings.
const MENU_OPS: Array[String] = ["tree", "edit", "list", "analyze"]
## The ops of one edit in editor_menu op=edit (engine/editor/session/record_batch.h).
const MENU_EDIT_OPS: Array[String] = ["set", "clear", "write", "add", "duplicate", "remove", "move"]
const MENU_EDIT_SCHEMA := {
	"type": "object",
	"properties": {
		"op": {"type": "string", "enum": MENU_EDIT_OPS},
		"id": {"description": "a record id, or the label an earlier add or duplicate gave with as"},
		"field": {"type": "string"},
		"value": {},
		"kind": {"type": "string"},
		"parent": {"description": "a record id (the screen's row id for a root window) or a label"},
		"position": {"type": "integer", "minimum": 0},
		"as": {"type": "string"},
	},
	"required": ["op"],
}
const MENU_PREVIEW_HANDLES: Array[String] = ["move", "left", "right", "top", "bottom", "top_left", "top_right",
		"bottom_left", "bottom_right"]


static func definitions() -> Array[McpToolDef]:
	return [
		McpToolDef.make("editor_state",
			"Read the editor's state: revision (moves with any change) and revisions (a counter "
			+ "per concern, each moving only with what it covers: project, files, findings, graph, "
			+ "documents, selection, output, operation, run, dialogs, preferences), the project "
			+ "(root, title, features, files: a page of the "
			+ "files the scan lists from files_offset, files_limit of them (file_count says how "
			+ "many there are), each with its path, name, kind and editable: whether the editor "
			+ "opens it), "
			+ "the requirements checklist with every row, the open documents, the "
			+ "selection (the primary and every selected record) and the clipboard's size, the "
			+ "file show_in_files last asked Files to show (reveal_file: path, serial, rename), the "
			+ "build, Play (state, pid, the running game's mcp_port (0 when no game with an "
			+ "endpoint runs), exit_code (the code the last game exited with on its own, null "
			+ "when none; nonzero is a play.crashed problem), the runtime the settings name as "
			+ "runtime_setting), what the last apply_project_settings came to "
			+ "(settings_result: its serial and the settings that could not be written), the "
			+ "import state (the dialog's preview: open, the choices a listing offers and the roots "
			+ "chosen, as imports take them, with_dependencies, the plan's rows (state selected or "
			+ "found, name, kind, source, destination, made_from, needed_by {file, record, field, "
			+ "reference, name}, found_in, selected, problem, rivals: the other places with a file "
			+ "for it), not_found (with needed_by), each of these four lists a page from import_offset "
			+ "(import_limit entries at most) with its count (choice_count, root_count, row_count, "
			+ "not_found_count), then not_followed, truncated, its diagnostics, changed when an import "
			+ "found the files changed; import_dependencies, the editor's setting; the project's "
			+ "imported sources), the problem counts, the recent projects and a page of the output "
			+ "lines (output_cursor / output_limit; next_cursor continues).",
			{
				"output_cursor": {"type": "integer", "minimum": 0, "default": 0},
				"output_limit": {"type": "integer", "minimum": 0, "maximum": PAGE_MAX, "default": PAGE_MAX},
				"import_offset": {"type": "integer", "minimum": 0, "default": 0},
				"import_limit": {"type": "integer", "minimum": 0, "maximum": PAGE_MAX, "default": PAGE_MAX},
				"files_offset": {"type": "integer", "minimum": 0, "default": 0},
				"files_limit": {"type": "integer", "minimum": 0, "maximum": PAGE_MAX, "default": PAGE_MAX},
			}, [], false),
		McpToolDef.make("editor_request",
			"Raise one typed editor request by kind, the same vocabulary the windows use: "
			+ "new_project {path, text=title}, open_project {path}, close_project, "
			+ "forget_recent {path}, rescan, apply_project_settings {settings:{serial?, title?, "
			+ "mission?, multiplayer?, retail_directory? (the game install, kept absolute (a "
			+ "relative path from the editor's working directory): the open project's, written to "
			+ "its .opennova/local.json, which opennova-project reads too, and kept as the "
			+ "editor's, where a project naming none starts), runtime_executable? ('' = the one "
			+ "packaged beside the editor), play_retail?}} (each setting left out stays as it is; what "
			+ "differs from the setting in effect is written, and editor_state's settings_result "
			+ "carries the serial back with the settings that could not be written), preview_import "
			+ "{paths, flag} (the import dialog planned: a loose file picked is chosen, an archive's members "
			+ "are listed to choose from; flag = with the files they need, found beside them or in "
			+ "the game install), plan_import {imports, flag} (the files chosen, planned again), "
			+ "set_import_dependencies {flag} (the editor's setting the dialog's 'Include the files "
			+ "these need' writes; an open preview is planned again with it), import_files "
			+ "{imports:[{path, entry, retail?, native? (a loose file copied as the game's own, with "
			+ "no import record)}]: the rows kept, each row's source as editor_state's import rows "
			+ "carry it, flag=replace} (with a preview open the files are planned again first: when "
			+ "that is not the plan shown nothing is written, import.changed, and the preview holds "
			+ "the new plan; a row it does not have is refused; every file is checked and staged "
			+ "under the project's cache before any is published, the whole selection or none of it "
			+ "as far as the disk allows, a failure while publishing said file by file; an .o3d's "
			+ "textures come only through its plan: a model naming one the import does not bring is "
			+ "import.texture_not_imported), cancel_import, "
			+ "create_missing {names=[the requirement roles whose files to make "
			+ "from scratch; none makes nothing; a file there already is refused, never overwritten]}, "
			+ "build, play, stop_play, create_file {path, text=kind token} (made from the name's "
			+ "requirement factory, else its kind's; a kind the editor does not edit, a font, is made "
			+ "and not opened; a texture is the checkerboard the game draws for a missing texture, in "
			+ "the format its name asks for: .tga or .mdt TGA, .pcx PCX, .dds DDS, another name "
			+ "refused), open_document {path, text?, edit?} (text: a record's locator, as editor_graph "
			+ "gives it, selected once open; edit.field: the field shown), show_in_files {path, flag} (Files selects the file and "
			+ "scrolls to it, a file the editor does not open included; flag = and asks its new name, "
			+ "Rename...), reload_document, "
			+ "close_document, select_record {path, edit.row/"
			+ "kind/child, mode=replace|add|toggle (the selection stays inside one row)}, "
			+ "edit_record {path, edit:{operation (set, clear, write, add, duplicate, remove, move, "
			+ "paste, set_file_value), row, kind, child, parent (an add's or paste's owner, a "
			+ "move's destination), field, value, position (an index inside the owner's "
			+ "collection), coalesce, gesture (edits sharing one fold into one undo step until "
			+ "end_edit)}, or edits:[...] (a batch on one row, one undo step), flag = open the "
			+ "document first when it is not (a fix's edit)}, revert_to_saved {path, edit:{row, "
			+ "kind, child, field}, or edits:[...] (the same field of several records of one row): "
			+ "the Inspector's Revert to saved, each given back the value and presence the saved "
			+ "file holds, one undo step; refused when none has anything to go back to}, end_edit, "
			+ "copy "
			+ "and cut {path} (the selected records), paste {path, edit.parent/position, or "
			+ "after the selection}, duplicate {path} (each selected record right after itself, one "
			+ "step), save {path?} (the active document when no path; a file with no unsaved edits "
			+ "is written when its bytes differ from what it would write; a file that is not open "
			+ "is read, rewritten that way when it must be, and left closed; one that does not "
			+ "serialize is refused with the reason), save_all (every file "
			+ "with unsaved edits, past a failure), undo, redo, resolve_unsaved "
			+ "{unsaved_choice=save|discard|cancel}, rename_asset {path, text=new name}, "
			+ "assign_requirement {text=role, path}, reimport {path=one source or '', flag=force}, "
			+ "preview_retail_import {names?, flag} (the import dialog on the game install's files: the "
			+ "names alone, chosen, or every file listed to choose from; flag as for preview_import; "
			+ "their sources carry retail: true), reveal_path {path}, clear_output (empties "
			+ "the output lines, as Output's Clear does), quit. A "
			+ "problem's fixes (editor_problems) are requests of these kinds. The pickers "
			+ "are refused: pass paths directly. build and play return at once; editor_state "
			+ "shows the build stepping (editor_build waits instead). Closing or reloading a "
			+ "file with unsaved edits, a project switch, quit, build and play, an import that "
			+ "replaces a file with unsaved edits and a rename or assign that rewrites one (or "
			+ "renames it) wait on the unsaved-changes prompt: editor_state's unsaved_prompt names "
			+ "the action and the files, and resolve_unsaved save writes them, then the action runs "
			+ "(build, play, import, rename and assign offer no discard); rescan reads again only "
			+ "the open documents whose files changed outside the editor (one that no longer reads "
			+ "stays open, document.stale; one with unsaved edits keeps them, document.conflict, "
			+ "whose fix reloads it); a file made unsaved since the prompt opened renews it instead "
			+ "(the answer waits again), and an answer with no prompt open is refused. ok says the "
			+ "request parsed; outcome says what it came to: done "
			+ "(false when it was refused, did not finish, or waits on the prompt), "
			+ "unsaved_prompt and the findings it reported.",
			{
				"kind": {"type": "string"},
				"path": {"type": "string"},
				"text": {"type": "string"},
				"flag": {"type": "boolean"},
				"paths": {"type": "array", "items": {"type": "string"}},
				"names": {"type": "array", "items": {"type": "string"}},
				"imports": {
					"type": "array",
					"items": {
						"type": "object",
						"properties": {"path": {"type": "string"}, "entry": {"type": "string"}, "retail": {"type": "boolean"},
								"native": {"type": "boolean"}},
					},
				},
				"edit": EDIT_SCHEMA,
				"edits": {"type": "array", "items": EDIT_SCHEMA},
				"mode": {"type": "string", "enum": ["replace", "add", "toggle"]},
				"unsaved_choice": {"type": "string", "enum": ["save", "discard", "cancel"]},
				"settings": {
					"type": "object",
					"properties": {
						"serial": {"type": "integer", "minimum": 0},
						"title": {"type": "string"},
						"mission": {"type": "boolean"},
						"multiplayer": {"type": "boolean"},
						"retail_directory": {"type": "string"},
						"runtime_executable": {"type": "string"},
						"play_retail": {"type": "boolean"},
					},
				},
			}, ["kind"], true, BUILD_TIMEOUT_MS),
		McpToolDef.make("editor_document",
			"The documents. op=list {offset?, limit?}: the open documents and a page of the files "
			+ "the project's scan lists (files: path, name, kind, editable: whether the editor "
			+ "opens it; file_count is the total, file_offset the page's first); "
			+ "open/create/close/reload {path} (create takes kind for a bare .bin); rows "
			+ "{path?, offset?, limit?}: a page of rows (row_count is the total, "
			+ "file_state_changed whether the file-wide state changed since the last save) with "
			+ "their collections and records at every depth (ids, names, change since the last "
			+ "save: unchanged, changed or added); record {id}: one record's path, locator, owner "
			+ "and index, its change since the last save, its fields as they apply to it with "
			+ "values, what the format says of each (unit, description, the key the file writes as "
			+ "token, a ranged number's min/max/step, a colour's form, the group whose row it shares), "
			+ "choices (open_choices: any other value typed too), presence (optional fields), "
			+ "reference status and, for a field "
			+ "changed since the last save, changed with what the saved file holds (saved, null "
			+ "when the file lacks the record; saved_present for an optional field), and the "
			+ "collections it holds; choices {id, field, offset?, limit?}: the names a reference "
			+ "field's picker offers there (field, reference, scope; choices: name, kind, file, record, "
			+ "status as the field set to it would resolve, inert with reason where no lookup of the "
			+ "game finds it), a page of them (count is the total, offset the page's first); targets "
			+ "{id, field, offset?, limit?}: where its Go to leads with the value it holds (value; "
			+ "targets: label, file, locator, field, editable: whether the editor opens the file, else "
			+ "Files shows it), a page as choices; find {symbol, scope?}: a record by the name another file uses "
			+ "(scope: where the symbol is looked up, GAMETEXT.BIN/WepDes a key of that section, "
			+ "MAIN.MNU/STARTUP a window of that screen); search {path?, text, match_case?, offset?, limit?}: "
			+ "every field whose value as the Inspector shows it holds the text, in document order, a page "
			+ "of them (count is the total, offset the page's first; hits: id, address, record, locator, "
			+ "field, label, text, at); set "
			+ "{id, field, value} (the value the field holds already: no edit, no undo step, the "
			+ "document as clean as it was); clear {id, field}: an optional field left out of the file; write "
			+ "{id, field}: an optional field written again with the value it reads (a set of "
			+ "that same value leaves it out); revert {id, field}: the field given back the value "
			+ "and presence the saved file holds (the Inspector's Revert to saved), one undo step, "
			+ "refused when it has nothing to go back to; add "
			+ "{kind=record kind name, parent?=the owning row or record id (0 = a new row), "
			+ "position?} returns the new id; duplicate {id} (right after it), or with no id every "
			+ "selected record (one step, the copies selected); remove {id}; "
			+ "move {id, position, parent?=another owner in its row}; select {id, mode?="
			+ "replace|add|toggle} (the selection stays inside one row); copy and cut (the "
			+ "selected records); paste {parent?, position?} (default: after the selection); "
			+ "end_edit; save {path?} (the active document when no path), save_all, undo, redo. "
			+ "Record ops act on the active document: open "
			+ "first. A dirty document makes editor_build wait on the unsaved-changes prompt. "
			+ "open, create, close, "
			+ "reload and save fail with the request's findings when it was refused, and close "
			+ "or reload of a dirty document fails asking for editor_request resolve_unsaved.",
			{
				"op": {"type": "string", "enum": DOCUMENT_OPS},
				"path": {"type": "string"},
				"kind": {"type": "string"},
				"id": {"type": "integer", "minimum": 0},
				"field": {"type": "string"},
				"value": {},
				"symbol": {"type": "string"},
				"scope": {"type": "string"},
				"text": {"type": "string"},
				"match_case": {"type": "boolean", "default": false},
				"parent": {"type": "integer", "minimum": 0, "default": 0},
				"position": {"type": "integer", "minimum": 0},
				"mode": {"type": "string", "enum": ["replace", "add", "toggle"], "default": "replace"},
				"offset": {"type": "integer", "minimum": 0, "default": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": PAGE_MAX, "default": PAGE_DEFAULT},
			}, ["op"]),
		McpToolDef.make("editor_problems",
			"The project's findings (the requirements, the files the last Play's game did not "
			+ "find, the scan, the validators, the menu render check, the last build's own until "
			+ "the next build, the last action; opennova-project validate prints the same "
			+ "composition for the project on disk), shown as the Problems window "
			+ "shows them: errors, then warnings, then notes. Each has severity, code, message, "
			+ "the asset, field and record it concerns, what it is about (role, target, "
			+ "reference, scope, loader_arg: what the reference's loader picks the file by, a "
			+ "model texture row's type) and its fixes: [{label, detail, bulk, request}], each request an "
			+ "editor_request to pass as it is (a bulk fix joins a Fix all). severities filters "
			+ "to those levels; text matches the message, file, record, field and code without "
			+ "case; scope project, active_file or open_files; fixable keeps those with a fix; "
			+ "group none, file or kind (each problem then names its group, group_count says how "
			+ "many groups are shown, and groups lists those of the page's problems, each whole: "
			+ "key, title, first (the index of its first row among the shown, an offset to page "
			+ "to), count and counts); offset and limit page the rows shown. Answers total (every "
			+ "finding), shown (the matches), counts (every finding by severity) and the page of "
			+ "problems.",
			{
				"severities": {"type": "array", "items": {"type": "string", "enum": ["error", "warning", "info"]}},
				"text": {"type": "string"},
				"scope": {"type": "string", "enum": ["project", "active_file", "open_files"], "default": "project"},
				"fixable": {"type": "boolean", "default": false},
				"group": {"type": "string", "enum": ["none", "file", "kind"], "default": "none"},
				"offset": {"type": "integer", "minimum": 0, "default": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": PAGE_MAX, "default": PAGE_DEFAULT},
			}, [], false),
		McpToolDef.make("editor_build",
			"Pack the project to an immutable build directory and wait for it: refused while "
			+ "required files are missing; with unsaved documents it waits on the unsaved-changes "
			+ "prompt (editor_request resolve_unsaved save writes them and builds). Returns the "
			+ "build block (ok, dir, archives, diagnostics).",
			{}, [], true, BUILD_TIMEOUT_MS),
		McpToolDef.make("editor_play",
			"op=start: build, then run the game on the build (the runtime beside the editor, "
			+ "or this Godot binary in a source run) with its own MCP endpoint; the reply "
			+ "carries mcp_port, which game_state / game_menu / game_probe on that port drive "
			+ "(with unsaved documents it waits on the unsaved-changes prompt, as editor_build does). "
			+ "op=stop ends the game and waits; op=state reads the play block.",
			{
				"op": {"type": "string", "enum": PLAY_OPS},
			}, ["op"], true, BUILD_TIMEOUT_MS),
		McpToolDef.make("editor_graph",
			"The asset graph: every cross-reference the project's files carry, read from the "
			+ "engine's own parsed records. references, referrers, usages, missing, symbols and search "
			+ "answer a page of their list from offset?, limit? entries (count is the total, offset the "
			+ "page's first). op=references {path}: what a file names (each edge "
			+ "with its record, field, kind, value and resolution, a model texture row's loader_arg); "
			+ "referrers {path} or {kind, "
			+ "name, scope?}: who names a file, or a symbol of a kind (weapon, ammo, item, text_id, user_point, "
			+ "style_var, particle, menu_screen, menu_window) defined in scope (a symbol's own, as "
			+ "op=symbols gives it); usages {path} or {kind, name, scope?}: who uses a file, the "
			+ "referrers of it and of every symbol it defines (a string table's ids, a catalog's "
			+ "names), each edge with the locator of the record that makes it; missing: every reference that does "
			+ "not resolve; symbols "
			+ "{kind?}: the names the files define (each with the file, record, locator and field "
			+ "that define it, its scope, its line where its type knows it, and inert with "
			+ "inert_reason where no lookup finds it); search "
			+ "{text}: the files whose names and the symbols whose names hold the text "
			+ "(without case), files first, each with usages (a file's usages, a symbol's uses that reach "
			+ "exactly it); "
			+ "rename {path, name}: "
			+ "rename a file with every "
			+ "reference rewritten, refused with reasons when a site cannot be rewritten, waiting "
			+ "on the unsaved-changes prompt when it would rewrite a file with unsaved edits; rename_symbol "
			+ "{path, locator, field, name, dry_run?}: rename a name a file defines (path, locator and field as "
			+ "op=symbols lists them) with every use that reaches it, in every file, on disk (not undoable), "
			+ "refused with reasons naming the file (a use the editor cannot rewrite, a name that scope has, one "
			+ "too long for a field), waiting on the unsaved-changes prompt as rename does; dry_run: the plan "
			+ "alone (the view's rename_preview: sites with before and after, refusals, ok); assign "
			+ "{role, path}: rename a file of the right kind to a requirement's name.",
			{
				"op": {"type": "string", "enum": GRAPH_OPS},
				"path": {"type": "string"},
				"kind": {"type": "string"},
				"name": {"type": "string"},
				"scope": {"type": "string"},
				"role": {"type": "string"},
				"text": {"type": "string"},
				"locator": {"type": "string"},
				"field": {"type": "string"},
				"dry_run": {"type": "boolean", "default": false},
				"offset": {"type": "integer", "minimum": 0, "default": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": PAGE_MAX, "default": PAGE_DEFAULT},
			}, ["op"]),
		McpToolDef.make("editor_menu_preview",
			"The menu preview, headless included: the previewed screen (the last menu screen "
			+ "selected) as the game would draw it were the menu saved now, the open stylesheets "
			+ "and string tables standing in for their files. op=state: status (no_project, "
			+ "no_menu, no_screen, unserializable, screen_missing, ready) and its message, the "
			+ "menu path and screen, the revision shown and whether it is current, the device "
			+ "size, the options, the files the screen names that the project does not have "
			+ "(missing), those it has that did not load (unreadable: a font or string table "
			+ "that does not parse, a texture that does not decode) and widget_count; rects "
			+ "{offset?, limit?}: a page of the widgets as the runtime placed them (index, "
			+ "id, name, type, shown, disabled, rect and local "
			+ "[left, top, right, bottom] in 800x600 design units, text, font, text_color); hit "
			+ "{x, y}: the widget the game's hit test finds at a design-space point (index -1 "
			+ "for none); options {width?, height?, show_hidden?, force_id?, force_state?="
			+ "normal|mouseover|selected|disabled, checked?, popup_open?, focus?}: how it draws "
			+ "(force_id holds that window in the state, checked, its list open, focused), then "
			+ "the state; drag {id, handle, dx, dy, snap?=true}: a window of the previewed screen "
			+ "moved (handle move) or resized by an edge or corner handle (left, right, top, "
			+ "bottom, top_left, top_right, bottom_left, bottom_right) by dx, dy design units, the "
			+ "moved edges snapped to the grid of 8, written as the preview window's drag writes "
			+ "its POSITION (the fewest edges that land it there), one undo step, then the state; "
			+ "nudge {id, dx, dy}: a move with no snap (the arrow keys); a drag or nudge of a selected "
			+ "window moves every selected window of the screen with it; arrange {ids, arrange}: "
			+ "the windows aligned to the first one's edge or centre (align_left, align_right, "
			+ "align_top, align_bottom, align_horizontal_centers, align_vertical_centers), spread "
			+ "with equal gaps (distribute_horizontally, distribute_vertically: three or more), or "
			+ "reordered among their siblings, a later sibling drawn over an earlier one "
			+ "(bring_to_front, bring_forward, send_backward, send_to_back), one undo step, then the "
			+ "state; notes {path?, screen?, "
			+ "offset?, limit?}: a page of the "
			+ "frame compiler's notes on the previewed screen, or with path and screen (a screen "
			+ "row id) on that screen as the render check compiled it (index, window_id, id: the "
			+ "record the note sits on, name, code, finding: menu.render.<code>, basis: witnessed, "
			+ "port_policy or deferred, severity: warning or info as a Problems row, preview when "
			+ "only the preview shows it, subject, list, record, field, message); render {path, "
			+ "screen, offset?, limit?}: that screen as the render check compiled it headless "
			+ "with the last validation, in the state's schema with a page of its widgets and "
			+ "note_count (the preview's parity read).",
			{
				"op": {"type": "string", "enum": MENU_PREVIEW_OPS},
				"x": {"type": "number"},
				"y": {"type": "number"},
				"path": {"type": "string"},
				"screen": {"type": "integer", "minimum": 1},
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
			}, ["op"]),
		McpToolDef.make("editor_model_preview",
			"The model preview, headless included: the previewed model (the last model document "
			+ "made active) as the game would draw it were it saved now, through the runtime's own "
			+ "renderer, its textures read from the project. op=state: status (no_project, no_model, "
			+ "unserializable, unreadable, ready) and its message, the path, the revision shown and "
			+ "whether it is current, builds (how many times the device built the model: an edit "
			+ "of a user point builds nothing), the device size, the options, lod {shown, auto: "
			+ "the level the game draws at the camera's distance, count, projected_px, "
			+ "thresholds}, camera {target, yaw, pitch, distance, fov: the game's horizontal "
			+ "field of view}, sphere {center, radius}, registers [{name, value}], user_points "
			+ "overlays [{kind: user_point|light|pivot, index, id: its record while the preview "
			+ "is current, name, part, position, screen: device pixels or null, direction, radius, "
			+ "cone, color}] (positions in the preview's space: the model's axes with x "
			+ "mirrored, y up), clock {time_ms, playing}; options {lod?: a level or \"auto\", "
			+ "ctrl?: {register: value}, the whole held set, playing?, overlays?: {user_points, "
			+ "lights, pivots}, time_ms?: a seek of the clock, rig_model?: the model an animation "
			+ "plays on (\"\" the graphic of an item whose anim_def names the table), clip_ticks?: "
			+ "a seek of the clip clock}: how it draws, then the state; the state's animation (a "
			+ "clip or table previewed, else null): {table, clip, model, source, rig, key, "
			+ "variant, file, ticks, frame, length_ticks, loops, events [{frame, tick, trigger}]}; "
			+ "camera {yaw?, pitch?, distance?, target?: [x, y, z], frame?: true to look at the "
			+ "whole model, width?, height?: the device size}: then the state; hit {x, y}: the "
			+ "marker at a device point (index -1 for none), as a click on it would pick; drag "
			+ "{id, handle?=place|axis, x, y, snap?=0}: the marker of record id (a user point or a "
			+ "light) moved, or its axis turned (a user point, a spot light), to the point under "
			+ "device pixel x, y on the plane through the handle that faces the eye, the place "
			+ "snapped to snap metres on the file\'s axes (0: free), one undo step, then the state.",
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
			}, ["op"]),
		McpToolDef.make("editor_menu",
			"The menu tools: one menu (path: a project-relative path or a logical name; none is the "
			+ "previewed menu, else the active document when it is a menu; the same menu for every "
			+ "op, so the ids a tree answers name the records an edit changes). "
			+ "op=tree {path?, screen?, offset?, limit?}: "
			+ "its screens (id, name, the render check's status and whether it is current) and each "
			+ "screen's windows in pre-order (id, name, type, parent: the window holding it, 0 for a "
			+ "root; depth, index: the frame compiler's, text, lists: how many records of each list "
			+ "it holds, and while the render is current rect and local [left, top, right, bottom] in "
			+ "800x600 design units and shown), a page of them per screen (window_count the total; "
			+ "screen: one screen's row id); a closed menu answers as the last validation read it. "
			+ "op=edit {path?, edits: [{op: set|clear|write|add|duplicate|remove|move, id, field, "
			+ "value, kind, parent, position, as}]}: one batch on one screen, one undo step, nothing "
			+ "done when any edit is refused: id names the record (a record id, or the label an "
			+ "earlier add or duplicate of the batch gave with as), kind an add's record kind "
			+ "(window, action, sound, items.item, appearance, hotkey, ...), parent an add's owner "
			+ "(a window, the screen's row id for a root window, or a label) and a move's "
			+ "destination, position an index in the owner's list (an add goes at the end, a "
			+ "duplicate right after its record); a set of a screen's or a window's name follows "
			+ "into the ACTIONs that name it; answers made {label: id} and added (every new id in "
			+ "order). op=list {path?, id, list, records: [{field: value, ...}]}: the list of a "
			+ "record (list: its kind token, action, sound, items.item, hotkey, ...) replaced by "
			+ "these records, each added with its fields set in the order written, one undo step "
			+ "(an ACTION's type first: the fields it reads follow its verb). op=analyze {path?, "
			+ "severity?, offset?, limit?}: the menu's Problems rows, each with its source (graph: "
			+ "the asset graph's references; render: the render check's compiler notes; menu: the "
			+ "menu's own checks; ...), the counts by severity and by source, and each screen's "
			+ "render status, note count and problem count. The menu opens when it is not open "
			+ "(edit, list). The preview is editor_menu_preview; a picture is editor_screenshot.",
			{
				"op": {"type": "string", "enum": MENU_OPS},
				"path": {"type": "string"},
				"screen": {"type": "integer", "minimum": 1},
				"edits": {"type": "array", "items": MENU_EDIT_SCHEMA},
				"id": {"type": "integer", "minimum": 1},
				"list": {"type": "string"},
				"records": {"type": "array", "items": {"type": "object"}},
				"severity": {"type": "string", "enum": ["error", "warning", "info"]},
				"offset": {"type": "integer", "minimum": 0, "default": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": PAGE_MAX, "default": PAGE_MAX},
			}, ["op"], true, BUILD_TIMEOUT_MS),
		McpToolDef.make("editor_screenshot",
			"Capture the editor window (its ImGui workspace, the menu preview and the model preview).",
			{
				"max_dim": {"type": "integer", "minimum": 64, "maximum": 4096, "default": 1280},
				"format": {"type": "string", "enum": ["webp", "png"], "default": "webp"},
				"quality": {"type": "number", "minimum": 0.1, "maximum": 1.0, "default": 0.8},
			}, [], true, SCREENSHOT_TIMEOUT_MS),
		McpToolDef.make("editor_logs",
			"The editor MCP's own log (sources server and script); the editor's output lines "
			+ "(the build log, the running game's log) are editor_state's output page.",
			{
				"cursor": {"type": "integer", "minimum": 0},
				"limit": {"type": "integer", "minimum": 1, "maximum": 2000, "default": 200},
			}, [], false),
	]
