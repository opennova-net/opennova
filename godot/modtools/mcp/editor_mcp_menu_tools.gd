class_name EditorMcpMenuTools
extends RefCounted

## The menu-authoring MCP tools: agents design .mnu menus through the Menus
## workspace's own seams — apply_edit per-field undo, snapshot-undo batches,
## the live WYSIWYG canvas, and the Interactive preview whose buttons dispatch
## the real authored Actions. Vocabulary per CONTEXT.md: a SCREEN is one
## full-canvas layout; every tree node is a WINDOW; a WIDGET is a typed
## Window; an ACTION is behavior the file expresses (navigate/show-hide/pop/
## url); a COMMAND is game behavior the engine binds to a widget's NAME.

const ROW_CAP := 50
const RECT_CAP := 100

const WIDGET_PROPS: Array[String] = [
	"name", "rect", "text", "string_type", "font", "flags", "group",
	"datasource", "orientation", "table_count", "table_spacing",
	"appearances", "frame",
]
# Widget types the original engine expects APPEARANCE state rows on (every
# shipped pressable carries them; text buttons use four EMPTY state rows).
const PRESSABLE_TYPES: Array[String] = ["BUTTON", "GOTO", "CHECKBOX", "RADIO", "COMBOBOX", "SPINLIST", "MULTI"]
const EMPTY_STATE_APPEARANCES: Array = [
	{ "state": "default" }, { "state": "mouseover" }, { "state": "selected" }, { "state": "disabled" },
]
const SCREEN_PROPS: Array[String] = ["name", "music_var", "text_rsrc", "cursor_file"]
const ACTION_TYPES: Array[String] = ["screen", "window", "pop_screen", "pop", "url", "quit_game", "quit"]
const WINDOW_STATES: Array[String] = ["", "show", "hide", "toggle"]
const COLOR_SLOTS: Array[String] = [
	"default_fg", "default_bg", "mouseover_fg", "mouseover_bg",
	"selected_fg", "selected_bg", "disabled_fg", "disabled_bg",
]
const TEXTURE_SLOTS: Array[String] = ["default", "mouseover", "selected", "disabled"]
const LIST_OPS: Array[String] = [
	"item_add", "item_remove", "item_move", "item_field",
	"header_add", "header_remove", "header_field",
	"body_add", "body_remove", "body_field",
	"subst_add", "subst_remove", "subst_field",
]

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(_def("get_menu",
			"The open Menu document (active tab of the Menus workspace): tabs (+active), menu_size, the visible Screen, the selection, and per-Screen widget trees — every Window node carries {id, type, name, rect [x,y,w,h] in menu-space pixels (parent-relative; child order = z-order), text (+string_type: \"id\" means text is a string-table key in the screen's text_rsrc), font, flags, group} plus action/sound/item counts. detail=\"full\" inlines actions, sounds and items. screen=<name> returns just that Screen; widget=<id> returns ONE widget's complete card instead (all 8 color slots, 4 texture slots, actions, sounds, items, table columns, absolute rect). Widget NAMES are Command hooks — the engine binds game behavior to them. %VAR% color/font values are stylesheet references. Requires the Menus workspace (open_in_workspace workspace=\"mnu\").",
			{
				"screen": { "type": "string" },
				"widget": { "type": "integer" },
				"detail": { "type": "string", "enum": ["summary", "full"], "default": "summary" },
				"max_widgets": { "type": "integer", "default": 500, "minimum": 1, "maximum": 2000 },
			}), Callable(self, "_tool_get_menu"))
	registry.register(_def("menu_tabs",
			"Manage the Menus workspace's document tabs (menus are MULTIDOC — each tab is one .mnu with its own dirty flag and undo history). op=\"new\": a fresh Untitled menu in its own tab (the start of a build-from-scratch flow). op=\"activate\" {index}: switches the active tab — undo/redo and every menu tool act on the ACTIVE tab. op=\"close\" {index}: refused while the tab has unsaved changes unless discard=true (closing the last tab seeds a fresh Untitled one). get_menu lists the tabs; opening files is open_in_workspace's job (it activates an existing tab instead of reopening).",
			{
				"op": { "type": "string", "enum": ["new", "activate", "close"] },
				"index": { "type": "integer" },
				"discard": { "type": "boolean", "default": false },
			}, ["op"]), Callable(self, "_tool_menu_tabs"))
	registry.register(_def("edit_menu_screen",
			"One Screen operation per call. add={name, background?, frame?}: a new Screen with a game-shaped root (a MAIN window with full position and an appearance row — the original engine crashes without them); text_rsrc/cursor/music_var auto-copy from the first Screen. background = a .tga drawn as the root's backdrop (e.g. letterbox.tga, what shipped screens use over the main menu); WITHOUT it the root keeps type=\"custom\" — an ENGINE-painted backdrop, which on the main menu means the bink video shows through. frame = {stencil, stencil_size?, brush, monogram?} border assets that DRAW_FRAME children render with (options.mnu uses BORDER2.tga/BOXTILE.tga/MONOGRAM.tga). delete={screen}: shows then deletes that Screen and everything on it (refused for the last one). set={screen, props}: props from {name, music_var (int), text_rsrc, cursor_file, background, frame} — one undo step per field. show={screen}: makes that Screen visible on the authoring canvas and selects it. screen = a Screen name or id from get_menu. Marks the menu dirty (except show); never save unless asked.",
			{
				"add": { "type": "object", "properties": { "name": { "type": "string" }, "background": { "type": "string" }, "frame": { "type": "object" } }, "required": ["name"] },
				"delete": { "type": ["string", "integer"] },
				"set": { "type": "object", "properties": { "screen": { "type": ["integer", "string"] }, "props": { "type": "object" } }, "required": ["screen", "props"] },
				"show": { "type": ["string", "integer"] },
			}), Callable(self, "_tool_edit_screen"))
	registry.register(_def("add_menu_widgets",
			"BATCH-create Windows with initial properties — the whole batch is ONE undo step. rows: [{parent (a widget id, or a Screen name/id to add at that Screen's root), type (widget type NAME: WINDOW, STATIC, BUTTON, EDIT, MULTILINE_EDIT, LIST, CHECKBOX, RADIO, COMBOBOX, SCROLL, TABLE, SPINLIST, MULTI, MAP, GLOBE, LABEL, GOTO, MARQUEE_WND), rect [x,y,w,h] (menu-space pixels, parent-relative; later siblings draw on top; w/h null or -1 = AUTO-SIZE, which box-art toggles and labels need — an explicit width STRETCHES appearance art in the game), name?, text?, string_type? (\"id\"=string-table key, \"\"=literal), font? (.fnt name), flags? (int bitmask), group? (radio group), appearances? (per-state rows [{state, type?, value?, map_state?, height?}])}]. Game-proven shaping: BUTTON/GOTO rows without appearances get the four EMPTY state rows shipped text buttons carry; CHECKBOX/RADIO need image rows (map_state 0..3 state-strip art like btn5.tga) or the game draws nothing — pass appearances; give text colors via font %DEF_TEXT_*% vars or edit_menu_widget color (colorless text renders unreadable in the game); a RADIO's own text clips to its art — pair an art-only radio with a sibling label STATIC instead. NAMES are Command hooks — reuse shipped names exactly when reproducing shipped menus. The batch validates up front: an unknown type or parent rejects the whole call. Max 50 rows. Wire navigation afterwards with set_widget_actions. Marks the menu dirty.",
			{
				"rows": { "type": "array", "minItems": 1, "maxItems": 50, "items": { "type": "object", "properties": {
					"parent": { "type": ["integer", "string"] },
					"type": { "type": "string" },
					"rect": { "type": "array", "items": { "type": ["number", "null"] }, "minItems": 4, "maxItems": 4 },
					"name": { "type": "string" }, "text": { "type": "string" }, "string_type": { "type": "string" },
					"font": { "type": "string" }, "flags": { "type": "integer" }, "group": { "type": "integer" },
					"appearances": { "type": "array", "items": { "type": "object" } },
				}, "required": ["parent", "type", "rect"] } },
			}, ["rows"], { "timeout_ms": 60000 }), Callable(self, "_tool_add_widgets"))
	registry.register(_def("edit_menu_widget",
			"Edit existing Windows — exactly one op per call. set={id, props}: props from {name (CAUTION: names are Command hooks — renaming a shipped widget can sever its game behavior), rect [x,y,w,h] (w/h null or -1 = auto-size), text, string_type, font, flags (1 Hidden, 2 Disabled, 4 Checked, 8 Draw Frame, 16 Modal, 32 Read Only), group, datasource, orientation (HORIZONTAL|VERTICAL), color {slot 0-7 or name default_fg/default_bg/mouseover_fg/mouseover_bg/selected_fg/selected_bg/disabled_fg/disabled_bg, value}, texture {slot 0-3 or name default/mouseover/selected/disabled, value}, appearances [{state, type?, value?, map_state?, height?}] (REPLACES all per-state rows — the only way to author shipped empty-state rows or custom/backdrop rows; the slot setters always write type=\"image\"), frame {stencil, stencil_size?, brush, monogram?} (border assets DRAW_FRAME children render with — normally on a screen's root window), table_count, table_spacing} — one undo step per field; values like %TITLE_COLOR% are stylesheet references, preserve them verbatim. Actions/sounds belong to set_widget_actions. move_rects={rows:[{id, rect}]}: a layout pass — up to 100 rects in ONE undo step. reparent={id, parent (widget id or Screen name), index}: moves a Window in the tree (index = position among the new parent's children = z-order). delete={id}: removes the Window and its subtree (Screens and a Screen's root window are not deletable — Screens go through edit_menu_screen). Marks the menu dirty.",
			{
				"set": { "type": "object", "properties": { "id": { "type": "integer" }, "props": { "type": "object" } }, "required": ["id", "props"] },
				"move_rects": { "type": "object", "properties": { "rows": { "type": "array", "maxItems": 100, "items": { "type": "object", "properties": { "id": { "type": "integer" }, "rect": { "type": "array", "items": { "type": "number" }, "minItems": 4, "maxItems": 4 } }, "required": ["id", "rect"] } } }, "required": ["rows"] },
				"reparent": { "type": "object", "properties": { "id": { "type": "integer" }, "parent": { "type": ["integer", "string"] }, "index": { "type": "integer", "default": -1 } }, "required": ["id", "parent"] },
				"delete": { "type": "integer" },
			}), Callable(self, "_tool_edit_widget"))
	registry.register(_def("set_widget_actions",
			"REPLACE a Window's Action list and/or sound list — the navigation and audio wiring. Actions are the ENTIRE behavior vocabulary the .mnu format carries; anything else a button does is a Command the engine binds to the widget's NAME. actions: full replacement list of [{type: \"screen\" (navigate; target=Screen name; file=another .mnu for a cross-menu jump) | \"window\" (show/hide a named Window on the same Screen — the Tab pattern; target=widget name, state=SHOW|HIDE|TOGGLE) | \"pop_screen\" (back) | \"url\" (target=address, external_browser?) | \"quit_game\", target?, state?, file?, external_browser?}]. Actions run in order on press. The game REQUIRES file= on every screen action (even a same-screen-file jump — an empty file crashes the original engine): for a target in THIS document with no file, the tool auto-fills the menu's own filename; an Untitled tab is allowed but warns (save_menu, then re-wire to bake it — analyze_menu's game_safety flags it meanwhile). Validation: in-document screen targets must exist (error lists Screens); cross-file and unmatched window targets warn; type tokens are case-insensitive (shipped files use SCREEN/POP_SCREEN), stored canonical lowercase. sounds: full replacement list of [{state, trigger, file}] — triggers are sound-set names in the menu .lwf (MOUSE_OVER, CLICK_SELECT, ...). Each list replace is one undo step. Marks the menu dirty.",
			{
				"id": { "type": "integer" },
				"actions": { "type": "array", "items": { "type": "object", "properties": {
					"type": { "type": "string" }, "target": { "type": "string" }, "state": { "type": "string" },
					"file": { "type": "string" }, "external_browser": { "type": "boolean" } }, "required": ["type"] } },
				"sounds": { "type": "array", "items": { "type": "object", "properties": {
					"state": { "type": "string" }, "trigger": { "type": "string" }, "file": { "type": "string" } } } },
			}, ["id"]), Callable(self, "_tool_set_actions"))
	registry.register(_def("edit_widget_items",
			"Edit a data widget's rows — List/Combo/SpinList/Multi items and Table columns. One op per call, each one undo step: item_add={row {type, value, text}}, item_remove={index}, item_move={from, to}, item_field={index, key, value}; Table columns: header_add/header_remove/header_field (rows {column, width, justify, vjustify, sort, text}), body_add/body_remove/body_field (rows {column, justify, vjustify, bitmap_draw, scale_bitmap, bitmap_flags}), subst_add/subst_remove/subst_field (rows {column, value, is_file, file}); set_table={count?, spacing?} sets a Table's column count/spacing. Returns the updated list. Marks the menu dirty.",
			{
				"id": { "type": "integer" },
				"op": { "type": "string", "enum": ["item_add", "item_remove", "item_move", "item_field",
					"header_add", "header_remove", "header_field", "body_add", "body_remove", "body_field",
					"subst_add", "subst_remove", "subst_field", "set_table"] },
				"row": { "type": "object" }, "index": { "type": "integer" },
				"from": { "type": "integer" }, "to": { "type": "integer" },
				"key": { "type": "string" }, "value": {},
				"count": { "type": "integer" }, "spacing": { "type": "integer" },
			}, ["id", "op"]), Callable(self, "_tool_edit_items"))
	registry.register(_def("preview_menu",
			"Drive the Interactive preview — the menu PLAYS inside the canvas: navigators wire up, pressing runs window show/hide and screen Actions, while external behavior (quit, URL, cross-menu jumps) is sandboxed to no-ops. No document edits ever result, but ALL menu editing tools are locked while it plays. Ops: on (start at the visible Screen) | off (back to authoring) | show={screen} (jump the preview, no back-stack push) | press={widget id or name, resolved on the current Screen} — your substitute for clicking: activates a Button/Checkbox/Radio/Goto exactly as a click would, runs its authored Actions, returns the resulting visible Screen and the actions fired | back (pop the navigation stack) | status. Follow with menu_screenshot to see the result. Hotkeys do not exist in the preview.",
			{
				"op": { "type": "string", "enum": ["on", "off", "show", "press", "back", "status"] },
				"screen": { "type": "string" },
				"widget": { "type": ["integer", "string"] },
			}, ["op"]), Callable(self, "_tool_preview"))
	registry.register(_def("menu_screenshot",
			"Capture the Menus WYSIWYG board as an image (cropped from the editor window). Optional screen=<name> shows that Screen first (authoring mode only — while the Interactive preview plays, navigate with preview_menu and call this with no args). The Menus workspace must be the ACTIVE workspace (open_in_workspace workspace=\"mnu\" activates it) and the editor must be windowed. Caption carries document, visible Screen, and interactive state. get_menu rects are menu-space pixels; the image is the letterboxed board.",
			{
				"screen": { "type": "string" },
				"max_dim": { "type": "integer", "default": 1280, "minimum": 64, "maximum": 4096 },
				"format": { "type": "string", "enum": ["webp", "png"], "default": "webp" },
				"quality": { "type": "number", "default": 0.8 },
			}, [], { "timeout_ms": 30000 }), Callable(self, "_tool_menu_screenshot"))
	# NOTE: the result's game_safety block applies corpus rules proven against the
	# original engine (crash + visual classes from live debugging) — treat its
	# errors as must-fix before a file ships to the game.
	registry.register(_def("analyze_menu",
			"Composition study of a Menu — the open one (no args, active tab) or ANY .mnu by name/path (read-only; nothing opens in the editor). Returns menu_size, per-Screen summaries, a widget-type histogram, the Action graph (Screen -> Screen edges incl. cross-file jumps and pops, plus per-Screen window show/hide counts), fonts in use, the text_rsrc string tables with id-key/literal text counts, likely Command hooks — named pressable widgets WITHOUT authored Actions, where the engine binds game behavior by name — and game_safety: findings from corpus rules proven against the ORIGINAL game engine (screen actions without file= and malformed screen roots crash it; missing text colors/appearance rows/frame assets render wrong). Fix every game_safety error before a save ships to the game; use the study output to author in a shipped menu's style.",
			{
				"path": { "type": "string" },
				"top": { "type": "integer", "default": 15, "minimum": 1, "maximum": 50 },
			}, [], { "timeout_ms": 60000 }), Callable(self, "_tool_analyze"))
	registry.register(_def("save_menu",
			"Write the ACTIVE menu tab to disk. ONLY call this when the user explicitly asked to save. No args: saves to the tab's current path (errors if the tab is Untitled — pass path). path: a filename (\"my_menu.mnu\", written into the mounted resource root) or an absolute path; must end in .mnu and becomes the tab's current path. Clears that tab's dirty flag; its undo history survives. Other tabs are untouched.",
			{ "path": { "type": "string" } }), Callable(self, "_tool_save"))


static func _def(name: String, description: String, properties := {}, required: Array = [], extra := {}) -> Dictionary:
	var schema := { "type": "object", "properties": properties }
	if not required.is_empty():
		schema["required"] = required
	var def := { "name": name, "description": description, "input_schema": schema }
	def.merge(extra)
	return def


# --- guards / helpers -------------------------------------------------------------

## The Menus workspace with a mounted editor and an active document, or an
## error result. Menus have no sim; the gate is presence (a pristine Untitled
## tab counts and is editable).
func _require_menu(ctx: McpToolContext) -> Dictionary:
	var ws: Variant = ctx.workspace("mnu")
	if ws == null:
		return { "error": "The Menus workspace is unavailable — is ONED fully booted?" }
	var editor: Variant = ws.get_editor_document()
	if editor == null or not is_instance_valid(editor):
		return { "error": "The Menus workspace has never been activated — open_in_workspace(workspace=\"mnu\", path=...) first (list candidates with list_assets(kind=\"menu\"))." }
	var doc: Variant = ws.get("_document")
	if doc == null or doc.get("resource") == null:
		return { "error": "No menu document — open_in_workspace(workspace=\"mnu\", path=...) first." }
	return { "ws": ws, "editor": editor, "doc": doc, "resource": doc.get("resource"), "canvas": editor.get("_canvas") }


## Menu present AND not playing: the Interactive preview locks every editing
## tool, mirroring the editor's own authoring lock (edits would rebuild the
## preview and drop its navigation state).
func _require_menu_editable(ctx: McpToolContext) -> Dictionary:
	var gate := _require_menu(ctx)
	if gate.has("error"):
		return gate
	var canvas: Variant = gate["canvas"]
	if canvas != null and canvas.is_interactive():
		return { "error": "The Interactive preview is playing — preview_menu(op=\"off\") first." }
	return gate


func _screen_id_named(resource: Variant, name: String) -> int:
	for sid in resource.get_screen_ids():
		if String(resource.get_screen_name(sid)) == name:
			return sid
	return -1


# A screen by name or id; -1 when absent or when the ref is not a name/id at
# all (an object/array here would otherwise stringify and "miss" silently).
func _resolve_screen(resource: Variant, value: Variant) -> int:
	if value is int or value is float:
		var sid := int(value)
		return sid if resource.widget_exists(sid) and resource.is_screen(sid) else -1
	if value is String or value is StringName:
		return _screen_id_named(resource, String(value))
	return -1


func _screen_names(resource: Variant) -> PackedStringArray:
	var names := PackedStringArray()
	for sid in resource.get_screen_ids():
		names.append(String(resource.get_screen_name(sid)))
	return names


# Widget type NAME -> enum int, probed from the live enum so the vocabulary
# can never drift from the format library.
func _type_table() -> Dictionary:
	var probe := NovaMnuDocument.new()
	var table := {}
	for t in range(NovaMnuDocument.TYPE_UNKNOWN):
		table[String(probe.get_widget_type_name(t)).to_upper()] = t
	# Friendly aliases for the two surprising canonical tokens.
	if table.has("COMBOBOX"):
		table["COMBO"] = table["COMBOBOX"]
	if table.has("MARQUEE_WND"):
		table["MARQUEE"] = table["MARQUEE_WND"]
	return table


# [x, y, w, h] in menu pixels; w/h may be null or -1 for auto-size (the writer
# then omits RIGHT/BOTTOM — how shipped menus size art toggles and labels).
func _rect_from(value: Variant) -> Variant:
	if not (value is Array) or (value as Array).size() != 4:
		return null
	var a: Array = value
	if not (a[0] is float or a[0] is int) or not (a[1] is float or a[1] is int):
		return null
	var w := -1.0
	if a[2] is float or a[2] is int:
		w = float(a[2])
	elif a[2] != null:
		return null
	var h := -1.0
	if a[3] is float or a[3] is int:
		h = float(a[3])
	elif a[3] != null:
		return null
	return Rect2(float(a[0]), float(a[1]), w, h)


static func _rect_out(rect: Rect2) -> Array:
	return [rect.position.x, rect.position.y, rect.size.x, rect.size.y]


# Pre-order search for a widget by name within a subtree (or the whole document
# when root_id < 0).
func _find_widget_named(resource: Variant, name: String, root_id: int = -1) -> int:
	var roots: Array = []
	if root_id >= 0:
		roots.append(root_id)
	else:
		for sid in resource.get_screen_ids():
			roots.append(sid)
	while not roots.is_empty():
		var id := int(roots.pop_front())
		if String(resource.get_widget_name(id)) == name:
			return id
		for child in resource.get_child_ids(id):
			roots.append(child)
	return -1


# One get_menu tree node; recurses children under a shared budget.
func _widget_node(resource: Variant, id: int, full: bool, budget: Dictionary) -> Variant:
	if int(budget["left"]) <= 0:
		return "<truncated>"
	budget["left"] = int(budget["left"]) - 1
	var node := {
		"id": id,
		"type": String(resource.get_widget_type_name(resource.get_widget_type(id))).to_upper(),
		"name": resource.get_widget_name(id),
		"rect": _rect_out(resource.get_window_rect(id)),
	}
	var text := String(resource.get_widget_text(id))
	if not text.is_empty():
		node["text"] = text
		node["string_type"] = resource.get_widget_string_type(id)
	var font := String(resource.get_widget_font(id))
	if not font.is_empty():
		node["font"] = font
	var flags := int(resource.get_widget_flags(id))
	if flags != 0:
		node["flags"] = flags
	var group := int(resource.get_widget_group(id))
	if group != 0:
		node["group"] = group
	var actions: Array = resource.get_widget_actions(id)
	var sounds: Array = resource.get_widget_sounds(id)
	var items := int(resource.get_item_count(id))
	if full:
		if not actions.is_empty():
			node["actions"] = actions
		if not sounds.is_empty():
			node["sounds"] = sounds
		if items > 0:
			node["items"] = resource.get_items(id)
	else:
		if not actions.is_empty():
			node["action_count"] = actions.size()
		if not sounds.is_empty():
			node["sound_count"] = sounds.size()
		if items > 0:
			node["item_count"] = items
	var children: Array = []
	for child in resource.get_child_ids(id):
		children.append(_widget_node(resource, child, full, budget))
	if not children.is_empty():
		node["children"] = children
	return node


func _tabs_block(ws: Variant) -> Dictionary:
	return { "tabs": DocumentTabRow.to_dict_rows(ws.get_document_tabs()), "active": ws.get_active_document_index() }


# --- read / tabs --------------------------------------------------------------------

func _tool_get_menu(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	var canvas: Variant = gate["canvas"]
	if args.has("widget"):
		var id := int(args["widget"])
		if not resource.widget_exists(id):
			return McpToolResult.error("No widget %d — get_menu lists the tree." % id)
		return _widget_card(resource, id)
	var full := String(args.get("detail", "summary")) == "full"
	var budget := { "left": clampi(int(args.get("max_widgets", 500)), 1, 2000) }
	var only := String(args.get("screen", ""))
	var screens: Array = []
	for sid in resource.get_screen_ids():
		var name := String(resource.get_screen_name(sid))
		if not only.is_empty() and name != only:
			continue
		screens.append({
			"id": sid,
			"name": name,
			"music_var": resource.get_screen_music_var(sid),
			"text_rsrc": resource.get_screen_text_rsrc(sid),
			"cursor_file": resource.get_screen_cursor_file(sid),
			"root_id": resource.get_screen_root_id(sid),
			"tree": _widget_node(resource, resource.get_screen_root_id(sid), full, budget),
		})
	if not only.is_empty() and screens.is_empty():
		return McpToolResult.error("No Screen named '%s'. Screens: %s" % [only, ", ".join(_screen_names(resource))])
	var size: Vector2i = resource.get_menu_size()
	return {
		"path": gate["doc"].get("current_path"),
		"documents": _tabs_block(ws),
		"menu_size": [size.x, size.y],
		"visible_screen": editor.get_visible_screen_name(),
		"interactive": canvas != null and canvas.is_interactive(),
		"selection": { "id": editor.get_selected_id() },
		"screens": screens,
		"truncated": int(budget["left"]) <= 0,
		"dirty": gate["doc"].get("is_dirty"),
	}


# The complete single-widget card (colors, textures, actions, sounds, items,
# table columns) for get_menu's widget mode.
func _widget_card(resource: Variant, id: int) -> Dictionary:
	var card := {
		"id": id,
		"type": String(resource.get_widget_type_name(resource.get_widget_type(id))).to_upper(),
		"name": resource.get_widget_name(id),
		"parent": resource.get_parent_id(id),
		"rect": _rect_out(resource.get_window_rect(id)),
		"text": resource.get_widget_text(id),
		"string_type": resource.get_widget_string_type(id),
		"font": resource.get_widget_font(id),
		"flags": resource.get_widget_flags(id),
		"flag_labels": resource.get_flag_labels(),
		"group": resource.get_widget_group(id),
		"datasource": resource.get_widget_datasource(id),
		"orientation": resource.get_widget_orientation(id),
		"actions": resource.get_widget_actions(id),
		"sounds": resource.get_widget_sounds(id),
		"children": resource.get_child_ids(id),
	}
	var colors := {}
	for slot in range(COLOR_SLOTS.size()):
		var value := String(resource.get_widget_color(id, slot))
		if not value.is_empty():
			colors[COLOR_SLOTS[slot]] = value
	card["colors"] = colors
	var textures := {}
	for slot in range(TEXTURE_SLOTS.size()):
		var value := String(resource.get_widget_texture(id, slot))
		if not value.is_empty():
			textures[TEXTURE_SLOTS[slot]] = value
	card["textures"] = textures
	card["appearances"] = resource.get_widget_appearances(id)
	var rect_flags := int(resource.get_window_rect_flags(id))
	if (rect_flags & NovaMnuDocument.RECT_HAS_RIGHT) == 0:
		card["auto_width"] = true
	if (rect_flags & NovaMnuDocument.RECT_HAS_BOTTOM) == 0:
		card["auto_height"] = true
	var frame: Dictionary = resource.get_window_frame(id)
	if not String(frame.get("stencil", "")).is_empty() or not String(frame.get("brush", "")).is_empty():
		card["frame"] = frame
	if resource.get_item_count(id) > 0:
		card["items"] = resource.get_items(id)
	if int(resource.get_table_column_count(id)) > 0:
		card["table"] = {
			"count": resource.get_table_column_count(id),
			"spacing": resource.get_table_column_spacing(id),
			"headers": resource.get_table_headers(id),
			"bodies": resource.get_table_bodies(id),
			"substs": resource.get_table_substs(id),
		}
	return card


func _tool_menu_tabs(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	match String(args.get("op", "")):
		"new":
			var err: Error = ws.new_current()
			if err != OK:
				return McpToolResult.error("New tab failed (%s)." % error_string(err))
		"activate":
			var index := int(args.get("index", -1))
			var tabs: Array[DocumentTabRow] = ws.get_document_tabs()
			if index < 0 or index >= tabs.size():
				return McpToolResult.error("No tab %d — get_menu lists tabs 0..%d." % [index, tabs.size() - 1])
			ws.activate_document(index)
		"close":
			var index := int(args.get("index", -1))
			var tabs: Array[DocumentTabRow] = ws.get_document_tabs()
			if index < 0 or index >= tabs.size():
				return McpToolResult.error("No tab %d — get_menu lists tabs 0..%d." % [index, tabs.size() - 1])
			var row: DocumentTabRow = tabs[index]
			if row.dirty and not bool(args.get("discard", false)):
				return McpToolResult.error("Tab %d (%s) has unsaved changes — save_menu first, or pass discard: true." % [index, row.label])
			ws.close_document(index)
		_:
			return McpToolResult.error("op must be new | activate | close.")
	await ctx.frames(1)
	return { "ok": true, "op": args["op"], "documents": _tabs_block(ws) }


# --- screens -------------------------------------------------------------------------

func _tool_edit_screen(args: Dictionary, ctx: McpToolContext) -> Variant:
	var ops := PackedStringArray()
	for op in ["add", "delete", "set", "show"]:
		if args.has(op):
			ops.append(op)
	if ops.size() != 1:
		return McpToolResult.error("Exactly one op per call (add | delete | set | show); got: %s" % [ops])
	var op := String(ops[0])
	var gate := _require_menu(ctx) if op == "show" else _require_menu_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	match op:
		"add":
			if not (args["add"] is Dictionary):
				return McpToolResult.error("add must be an object: {name: \"...\"}.")
			var name := String((args["add"] as Dictionary).get("name", "")).strip_edges()
			if name.is_empty():
				return McpToolResult.error("add.name is required.")
			if _screen_id_named(resource, name) >= 0:
				return McpToolResult.error("A Screen named '%s' already exists (names must be unique — they are navigation targets). Screens: %s" % [name, ", ".join(_screen_names(resource))])
			var sid: int = editor.add_screen_action(name)
			if sid < 0:
				return McpToolResult.error("Screen add rejected.")
			# Game-shaped completion: every shipped screen carries TEXT_RSRC,
			# CURSOR and a MUSICVAR — copy them from the first screen so the new
			# one is valid in the original engine out of the box.
			var screen_ids: Array = resource.get_screen_ids()
			if screen_ids.size() > 1 and int(screen_ids[0]) != sid:
				var first := int(screen_ids[0])
				var copies := {
					"music_var": resource.get_screen_music_var(first),
					"text_rsrc": String(resource.get_screen_text_rsrc(first)),
					"cursor": String(resource.get_screen_cursor_file(first)),
				}
				for prop in copies:
					if copies[prop] is String and String(copies[prop]).is_empty():
						continue
					editor.apply_edit({ "target": "screen", "id": sid, "prop": prop, "value": copies[prop] })
			var root_id := int(resource.get_screen_root_id(sid))
			var background := String((args["add"] as Dictionary).get("background", ""))
			if not background.is_empty():
				# An image backdrop covers whatever plays underneath (the main
				# menu's engine-painted bink shows through a "custom" root).
				editor.apply_edit({ "target": "widget", "id": root_id, "prop": "appearances",
						"value": [{ "state": "default", "type": "image", "value": background }] })
			var frame_spec: Variant = (args["add"] as Dictionary).get("frame")
			if frame_spec is Dictionary and not (frame_spec as Dictionary).is_empty():
				editor.apply_edit({ "target": "widget", "id": root_id, "prop": "frame", "value": frame_spec })
			return { "ok": true, "op": "add", "screen_id": sid, "root_id": root_id, "name": name, "screens": _screen_names(resource), "dirty": gate["doc"].get("is_dirty") }
		"delete":
			var sid := _resolve_screen(resource, args["delete"])
			if sid < 0:
				return McpToolResult.error("No Screen '%s'. Screens: %s" % [args["delete"], ", ".join(_screen_names(resource))])
			if resource.get_screen_count() <= 1:
				return McpToolResult.error("Never delete the last Screen — a Menu needs at least one.")
			var name := String(resource.get_screen_name(sid))
			ws.focus_screen_named(name)
			editor.delete_screen_action()
			await ctx.frames(1)
			if resource.widget_exists(sid):
				return McpToolResult.error("Screen delete rejected.")
			return { "ok": true, "op": "delete", "screens": _screen_names(resource), "dirty": gate["doc"].get("is_dirty") }
		"set":
			if not (args["set"] is Dictionary):
				return McpToolResult.error("set must be an object: {screen, props: {...}}.")
			var spec: Dictionary = args["set"]
			var sid := _resolve_screen(resource, spec.get("screen"))
			if sid < 0:
				return McpToolResult.error("No Screen '%s'. Screens: %s" % [spec.get("screen"), ", ".join(_screen_names(resource))])
			var props: Dictionary = spec.get("props", {}) if spec.get("props") is Dictionary else {}
			if props.is_empty():
				return McpToolResult.error("set.props is required: {name | music_var | text_rsrc | cursor_file | background | frame -> value}.")
			for key in props:
				var key_name := String(key)
				if not SCREEN_PROPS.has(key_name) and key_name != "background" and key_name != "frame":
					return McpToolResult.error("Unknown screen prop '%s'. Valid: %s, background, frame" % [key, ", ".join(SCREEN_PROPS)])
			if props.has("name"):
				var new_name := String(props["name"])
				var existing := _screen_id_named(resource, new_name)
				if existing >= 0 and existing != sid:
					return McpToolResult.error("A Screen named '%s' already exists." % new_name)
			var root_id := int(resource.get_screen_root_id(sid))
			for key in props:
				match String(key):
					"background":
						editor.apply_edit({ "target": "widget", "id": root_id, "prop": "appearances",
								"value": [{ "state": "default", "type": "image", "value": String(props[key]) }] })
					"frame":
						if not (props[key] is Dictionary):
							return McpToolResult.error("frame must be an object: {stencil, stencil_size?, brush, monogram?}.")
						editor.apply_edit({ "target": "widget", "id": root_id, "prop": "frame", "value": props[key] })
					"cursor_file":
						editor.apply_edit({ "target": "screen", "id": sid, "prop": "cursor", "value": props[key] })
					_:
						editor.apply_edit({ "target": "screen", "id": sid, "prop": String(key), "value": props[key] })
			return { "ok": true, "op": "set", "screen_id": sid, "name": resource.get_screen_name(sid), "dirty": gate["doc"].get("is_dirty") }
		"show":
			var sid := _resolve_screen(resource, args["show"])
			if sid < 0:
				return McpToolResult.error("No Screen '%s'. Screens: %s" % [args["show"], ", ".join(_screen_names(resource))])
			ws.focus_screen_named(String(resource.get_screen_name(sid)))
			await ctx.frames(1)
			return { "ok": true, "op": "show", "visible_screen": editor.get_visible_screen_name() }
	return McpToolResult.error("Unhandled op.")


# --- widgets -------------------------------------------------------------------------

func _tool_add_widgets(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	var rows: Array = args.get("rows", []) if args.get("rows") is Array else []
	if rows.is_empty():
		return McpToolResult.error("rows is required: [{parent, type, rect, ...}].")
	if rows.size() > ROW_CAP:
		return McpToolResult.error("Too many rows (%d) — max %d per call." % [rows.size(), ROW_CAP])
	var types := _type_table()
	var warnings: Array = []
	var seam_rows: Array = []
	for i in range(rows.size()):
		var row: Dictionary = rows[i]
		var type_name := String(row.get("type", "")).to_upper()
		if not types.has(type_name):
			return McpToolResult.error("Row %d: unknown widget type '%s'. Types: %s" % [i, row.get("type"), ", ".join(types.keys())])
		var parent: Variant = row.get("parent")
		var parent_id := -1
		if parent is int or parent is float:
			parent_id = int(parent)
			if not resource.widget_exists(parent_id):
				return McpToolResult.error("Row %d: no widget %d to parent under — get_menu lists ids." % [i, parent_id])
		else:
			parent_id = _screen_id_named(resource, String(parent))
			if parent_id < 0:
				return McpToolResult.error("Row %d: no Screen named '%s'. Screens: %s" % [i, parent, ", ".join(_screen_names(resource))])
		var rect: Variant = _rect_from(row.get("rect"))
		if rect == null:
			return McpToolResult.error("Row %d: rect must be [x, y, w, h] (w/h may be null or -1 for auto-size)." % i)
		var props := {}
		for key in ["name", "text", "string_type", "font", "flags", "group"]:
			if row.has(key):
				props[key] = row[key]
		# Game-safety shaping (proven in the real engine): every shipped pressable
		# carries APPEARANCE state rows — text buttons use four EMPTY rows; box
		# toggles need image rows (map_state 0..3 art) the caller must pick.
		if row.get("appearances") is Array and not (row.get("appearances") as Array).is_empty():
			props["appearances"] = row.get("appearances")
		elif type_name == "BUTTON" or type_name == "GOTO":
			props["appearances"] = EMPTY_STATE_APPEARANCES.duplicate(true)
		elif PRESSABLE_TYPES.has(type_name):
			warnings.append("row %d: %s has no appearance rows — the game draws its box from APPEARANCE image rows (map_state 0..3, e.g. btn5.tga); pass row.appearances" % [i, type_name])
		if type_name == "RADIO" and not String(row.get("text", "")).is_empty():
			warnings.append("row %d: the game clips a radio's own label to its art width — author the label as a sibling STATIC (the shipped pattern: art-only radio + label static)" % i)
		seam_rows.append({ "parent": parent_id, "type": types[type_name], "rect": rect, "props": props })
	var results: Array = editor.add_widgets_batch(seam_rows)
	await ctx.frames(1)
	var ids: Array = []
	var added := 0
	for result: Dictionary in results:
		if bool(result.get("ok", false)):
			ids.append(result["id"])
			added += 1
	return { "added": added, "failed": results.size() - added, "ids": ids, "rows": results,
			"warnings": warnings, "undo_steps": 1, "dirty": gate["doc"].get("is_dirty") }


func _tool_edit_widget(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	var ops := PackedStringArray()
	for op in ["set", "move_rects", "reparent", "delete"]:
		if args.has(op):
			ops.append(op)
	if ops.size() != 1:
		return McpToolResult.error("Exactly one op per call (set | move_rects | reparent | delete); got: %s" % [ops])
	match String(ops[0]):
		"set":
			if not (args["set"] is Dictionary):
				return McpToolResult.error("set must be an object: {id, props: {...}}.")
			var spec: Dictionary = args["set"]
			var id := int(spec.get("id", -1))
			if not resource.widget_exists(id):
				return McpToolResult.error("No widget %d — get_menu lists ids." % id)
			if resource.is_screen(id):
				return McpToolResult.error("Screens are edited with edit_menu_screen.")
			var props: Dictionary = spec.get("props", {}) if spec.get("props") is Dictionary else {}
			if props.is_empty():
				return McpToolResult.error("set.props is required.")
			for key in props:
				var name := String(key)
				if name == "actions" or name == "sounds":
					return McpToolResult.error("Actions and sounds are wired with set_widget_actions.")
				if not WIDGET_PROPS.has(name) and name != "color" and name != "texture":
					return McpToolResult.error("Unknown prop '%s'. Valid: %s, color {slot, value}, texture {slot, value}. Flags: %s" % [
							name, ", ".join(WIDGET_PROPS), resource.get_flag_labels()])
			for key in props:
				var name := String(key)
				match name:
					"color", "texture":
						var slot_spec: Dictionary = props[key] if props[key] is Dictionary else {}
						var slot := _resolve_slot(slot_spec.get("slot"), COLOR_SLOTS if name == "color" else TEXTURE_SLOTS)
						if slot < 0:
							return McpToolResult.error("%s.slot must be 0-%d or one of: %s" % [name,
									(COLOR_SLOTS if name == "color" else TEXTURE_SLOTS).size() - 1,
									", ".join(COLOR_SLOTS if name == "color" else TEXTURE_SLOTS)])
						editor.apply_edit({ "target": "widget", "id": id, "prop": name, "slot": slot, "value": String(slot_spec.get("value", "")) })
					"rect":
						var rect: Variant = _rect_from(props[key])
						if rect == null:
							return McpToolResult.error("rect must be [x, y, w, h] (w/h may be null or -1 for auto-size).")
						editor.apply_edit({ "target": "widget", "id": id, "prop": "rect", "value": rect })
					"appearances":
						if not (props[key] is Array):
							return McpToolResult.error("appearances must be a list of rows: [{state, type?, value?, map_state?, height?}, ...] (replaces all rows; empty-state rows = text button, image rows with map_state 0..3 = state-strip art).")
						editor.apply_edit({ "target": "widget", "id": id, "prop": "appearances", "value": props[key] })
					"frame":
						if not (props[key] is Dictionary):
							return McpToolResult.error("frame must be an object: {stencil, stencil_size?, brush, monogram?} — DRAW_FRAME children render with the nearest ancestor's frame (put it on the screen's root window).")
						editor.apply_edit({ "target": "widget", "id": id, "prop": "frame", "value": props[key] })
					_:
						editor.apply_edit({ "target": "widget", "id": id, "prop": name, "value": props[key] })
			return { "ok": true, "op": "set", "widget": _widget_card(resource, id), "dirty": gate["doc"].get("is_dirty") }
		"move_rects":
			if not (args["move_rects"] is Dictionary):
				return McpToolResult.error("move_rects must be an object: {rows: [{id, rect}, ...]}.")
			var rows: Array = (args["move_rects"] as Dictionary).get("rows", []) if (args["move_rects"] as Dictionary).get("rows") is Array else []
			if rows.is_empty() or rows.size() > RECT_CAP:
				return McpToolResult.error("move_rects.rows must hold 1..%d rows." % RECT_CAP)
			var edits: Array = []
			for row_variant: Variant in rows:
				if not (row_variant is Dictionary):
					return McpToolResult.error("Each move_rects row must be an object {id, rect}.")
				var row: Dictionary = row_variant
				var id := int(row.get("id", -1))
				if not resource.widget_exists(id) or resource.is_screen(id):
					return McpToolResult.error("No movable widget %d." % id)
				var rect: Variant = _rect_from(row.get("rect"))
				if rect == null:
					return McpToolResult.error("Each row needs rect [x, y, w, h].")
				edits.append({ "id": id, "rect": rect })
			editor.apply_rect_batch(edits)
			return { "ok": true, "op": "move_rects", "moved": edits.size(), "undo_steps": 1, "dirty": gate["doc"].get("is_dirty") }
		"reparent":
			if not (args["reparent"] is Dictionary):
				return McpToolResult.error("reparent must be an object: {id, parent, index?}.")
			var spec: Dictionary = args["reparent"]
			var id := int(spec.get("id", -1))
			if not resource.widget_exists(id):
				return McpToolResult.error("No widget %d." % id)
			var parent: Variant = spec.get("parent")
			if not (parent is int or parent is float or parent is String or parent is StringName):
				return McpToolResult.error("reparent.parent must be a widget id or a Screen name.")
			var parent_id := int(parent) if (parent is int or parent is float) else _screen_id_named(resource, String(parent))
			if parent_id < 0 or not resource.widget_exists(parent_id):
				return McpToolResult.error("No reparent target '%s'." % [parent])
			if resource.is_screen(parent_id):
				parent_id = resource.get_screen_root_id(parent_id)
			var before_parent := int(resource.get_parent_id(id))
			editor.reparent_action(id, parent_id, int(spec.get("index", -1)))
			await ctx.frames(1)
			if int(resource.get_parent_id(id)) == before_parent and before_parent != parent_id:
				return McpToolResult.error("Reparent rejected (cycle, root window, or unknown target).")
			return { "ok": true, "op": "reparent", "parent": resource.get_parent_id(id), "dirty": gate["doc"].get("is_dirty") }
		"delete":
			if not (args["delete"] is int or args["delete"] is float):
				return McpToolResult.error("delete must be a widget id (get_menu lists ids).")
			var id := int(args["delete"])
			if not resource.widget_exists(id):
				return McpToolResult.error("No widget %d." % id)
			if resource.is_screen(id):
				return McpToolResult.error("Screens are deleted with edit_menu_screen.")
			if resource.is_screen(resource.get_parent_id(id)):
				return McpToolResult.error("A Screen's root window is not deletable — delete the Screen instead.")
			editor.select_widget(id)
			editor.delete_selection_action()
			await ctx.frames(1)
			if resource.widget_exists(id):
				return McpToolResult.error("Delete rejected.")
			return { "ok": true, "op": "delete", "dirty": gate["doc"].get("is_dirty") }
	return McpToolResult.error("Unhandled op.")


static func _resolve_slot(value: Variant, names: Array) -> int:
	if value is int or value is float:
		var slot := int(value)
		return slot if slot >= 0 and slot < names.size() else -1
	return names.find(String(value).to_lower())


func _tool_set_actions(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	var id := int(args.get("id", -1))
	if not resource.widget_exists(id) or resource.is_screen(id):
		return McpToolResult.error("No widget %d — get_menu lists ids." % id)
	if not args.has("actions") and not args.has("sounds"):
		return McpToolResult.error("Pass actions and/or sounds (full replacement lists).")
	var warnings: Array = []
	var undo_steps := 0
	if args.has("actions"):
		var actions: Array = args.get("actions", []) if args.get("actions") is Array else []
		var screen_of := _owning_screen(resource, id)
		var own_file := String(gate["doc"].get("current_path")).get_file()
		for i in range(actions.size()):
			var action: Dictionary = actions[i]
			var type := String(action.get("type", "")).to_lower()
			if not ACTION_TYPES.has(type):
				return McpToolResult.error("Action %d: unknown type '%s'. Types: %s" % [i, action.get("type"), ", ".join(ACTION_TYPES)])
			action["type"] = type # shipped files carry SCREEN/POP_SCREEN too; store canonical lowercase
			match type:
				"screen":
					var file := String(action.get("file", ""))
					var target := String(action.get("target", ""))
					if file.is_empty():
						if _screen_id_named(resource, target) < 0:
							return McpToolResult.error("Action %d: no Screen named '%s' in this Menu. Screens: %s (cross-menu jumps need file=<other.mnu>)." % [i, target, ", ".join(_screen_names(resource))])
						# The game requires file= on EVERY screen action — shipped
						# same-file jumps name their own file (mp.mnu does); an empty
						# file crashed the original engine. The in-editor preview
						# navigates same-file regardless, so an Untitled tab is
						# allowed with a warning and analyze_menu flags it until saved.
						if own_file.is_empty():
							warnings.append("action %d: this tab is Untitled — leaving file= empty; the GAME needs file= on screen actions, so save_menu then re-wire to auto-fill it (analyze_menu flags it meanwhile)" % i)
						else:
							action["file"] = own_file
							warnings.append("action %d: file auto-filled to '%s' (the game crashes on screen actions without file=)" % [i, own_file])
					elif own_file.is_empty() or file.nocasecmp_to(own_file) != 0:
						warnings.append("action %d: cross-menu target %s/%s not validated (jumps are host policy)" % [i, file, target])
				"window":
					var state := String(action.get("state", "")).to_lower()
					if not WINDOW_STATES.has(state):
						return McpToolResult.error("Action %d: window state must be SHOW | HIDE | TOGGLE." % i)
					var target := String(action.get("target", ""))
					if screen_of >= 0 and not target.is_empty() and _find_widget_named(resource, target, screen_of) < 0:
						warnings.append("action %d: no Window named '%s' on this Screen (resolved at runtime)" % [i, target])
		editor.apply_edit({ "target": "widget", "id": id, "prop": "actions", "value": actions })
		undo_steps += 1
	if args.has("sounds"):
		var sounds: Array = args.get("sounds", []) if args.get("sounds") is Array else []
		editor.apply_edit({ "target": "widget", "id": id, "prop": "sounds", "value": sounds })
		undo_steps += 1
	return {
		"ok": true,
		"actions": resource.get_widget_actions(id),
		"sounds": resource.get_widget_sounds(id),
		"warnings": warnings,
		"undo_steps": undo_steps,
		"dirty": gate["doc"].get("is_dirty"),
	}


# The screen id a widget lives on (walk up to the screen node), -1 when unrooted.
func _owning_screen(resource: Variant, id: int) -> int:
	var current := id
	while current >= 0 and not resource.is_screen(current):
		current = resource.get_parent_id(current)
	return current


func _tool_edit_items(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu_editable(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	var id := int(args.get("id", -1))
	if not resource.widget_exists(id) or resource.is_screen(id):
		return McpToolResult.error("No widget %d — get_menu lists ids." % id)
	var op := String(args.get("op", ""))
	if op != "set_table" and not LIST_OPS.has(op):
		return McpToolResult.error("Unknown op '%s'. Ops: %s, set_table" % [op, ", ".join(LIST_OPS)])
	# header_*/body_*/subst_*/set_table edit TABLE columns; on any other widget
	# type the document silently ignores them — refuse loudly instead.
	if not op.begins_with("item_"):
		var tname := String(resource.get_widget_type_name(resource.get_widget_type(id))).to_upper()
		if tname != "TABLE":
			return McpToolResult.error("%s edits TABLE columns, but widget %d is a %s — List/Combo/SpinList/Multi rows are edited with item_add / item_remove / item_move / item_field." % [op, id, tname])
	if op == "set_table":
		if args.has("count"):
			editor.apply_edit({ "target": "widget", "id": id, "prop": "table_count", "value": int(args["count"]) })
		if args.has("spacing"):
			editor.apply_edit({ "target": "widget", "id": id, "prop": "table_spacing", "value": int(args["spacing"]) })
		return { "ok": true, "op": op, "count": resource.get_table_column_count(id),
				"spacing": resource.get_table_column_spacing(id), "dirty": gate["doc"].get("is_dirty") }
	var sizes := {
		"item": resource.get_items(id).size(),
		"header": resource.get_table_headers(id).size(),
		"body": resource.get_table_bodies(id).size(),
		"subst": resource.get_table_substs(id).size(),
	}
	var family := op.get_slice("_", 0)
	var verb := op.get_slice("_", 1)
	var edit := { "id": id, "op": op }
	match verb:
		"add":
			if not (args.get("row") is Dictionary):
				return McpToolResult.error("%s needs row {…}." % op)
			edit["row"] = args["row"]
		"remove":
			var index := int(args.get("index", -1))
			if index < 0 or index >= int(sizes[family]):
				return McpToolResult.error("%s: index %d out of range (0..%d)." % [op, index, int(sizes[family]) - 1])
			edit["index"] = index
		"move":
			var from := int(args.get("from", -1))
			var to := int(args.get("to", -1))
			if from < 0 or from >= int(sizes[family]) or to < 0 or to >= int(sizes[family]):
				return McpToolResult.error("%s: from/to out of range (0..%d)." % [op, int(sizes[family]) - 1])
			edit["from"] = from
			edit["to"] = to
		"field":
			var index := int(args.get("index", -1))
			if index < 0 or index >= int(sizes[family]):
				return McpToolResult.error("%s: index %d out of range (0..%d)." % [op, index, int(sizes[family]) - 1])
			if String(args.get("key", "")).is_empty() or not args.has("value"):
				return McpToolResult.error("%s needs key and value." % op)
			edit["index"] = index
			edit["key"] = args["key"]
			edit["value"] = args["value"]
	editor.apply_list_edit(edit)
	var out := { "ok": true, "op": op, "dirty": gate["doc"].get("is_dirty") }
	match family:
		"item":
			out["items"] = resource.get_items(id)
		"header":
			out["headers"] = resource.get_table_headers(id)
		"body":
			out["bodies"] = resource.get_table_bodies(id)
		"subst":
			out["substs"] = resource.get_table_substs(id)
	return out


# --- preview / screenshot -------------------------------------------------------------

func _tool_preview(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var resource: Variant = gate["resource"]
	var canvas: Variant = gate["canvas"]
	if canvas == null:
		return McpToolResult.error("The Menus canvas is not mounted — activate the workspace (open_in_workspace workspace=\"mnu\").")
	match String(args.get("op", "")):
		"on":
			canvas.set_interactive(true)
			await ctx.frames(1)
			return { "interactive": true, "visible_screen": canvas.get_visible_screen_name() }
		"off":
			canvas.set_interactive(false)
			await ctx.frames(1)
			return { "interactive": false, "visible_screen": canvas.get_visible_screen_name() }
		"show":
			if not canvas.is_interactive():
				return McpToolResult.error("The preview is not playing — preview_menu(op=\"on\") first (authoring-canvas screens switch with edit_menu_screen show).")
			var screen_ref: Variant = args.get("screen", "")
			if not (screen_ref is String or screen_ref is StringName):
				return McpToolResult.error("show needs screen as a name string.")
			var name := String(screen_ref)
			if _screen_id_named(resource, name) < 0:
				return McpToolResult.error("No Screen named '%s'. Screens: %s" % [name, ", ".join(_screen_names(resource))])
			var preview: Variant = canvas.get("_preview")
			preview.show_screen(name)
			await ctx.frames(1)
			return { "interactive": true, "visible_screen": canvas.get_visible_screen_name() }
		"press":
			if not canvas.is_interactive():
				return McpToolResult.error("The preview is not playing — preview_menu(op=\"on\") first.")
			return await _preview_press(args, ctx, gate)
		"back":
			if not canvas.is_interactive():
				return McpToolResult.error("The preview is not playing — preview_menu(op=\"on\") first.")
			var preview: Variant = canvas.get("_preview")
			var popped: bool = preview.pop_screen()
			await ctx.frames(1)
			return { "popped": popped, "visible_screen": canvas.get_visible_screen_name() }
		"status":
			return {
				"interactive": canvas.is_interactive(),
				"visible_screen": canvas.get_visible_screen_name(),
				"screens": _screen_names(resource),
				"selection": { "id": editor.get_selected_id() },
			}
	return McpToolResult.error("op must be on | off | show | press | back | status.")


# Activate a live preview Control exactly as a click would (the hotkey
# trigger's pattern): toggle-mode buttons flip, plain buttons emit pressed
# (NovaMnuButton dispatches its authored Actions), Gotos trigger.
func _preview_press(args: Dictionary, ctx: McpToolContext, gate: Dictionary) -> Variant:
	var resource: Variant = gate["resource"]
	var canvas: Variant = gate["canvas"]
	var target: Variant = args.get("widget")
	if target == null:
		return McpToolResult.error("press needs widget (an id or a name on the current Screen).")
	var id := -1
	if target is int or target is float:
		id = int(target)
	elif target is String or target is StringName:
		var visible := _screen_id_named(resource, String(canvas.get_visible_screen_name()))
		id = _find_widget_named(resource, String(target), visible)
		if id < 0:
			id = _find_widget_named(resource, String(target))
	else:
		return McpToolResult.error("press widget must be an id or a name string.")
	if id < 0 or not resource.widget_exists(id):
		return McpToolResult.error("No widget '%s' — get_menu lists names and ids." % [target])
	var controls: Dictionary = canvas.get("_id_to_control") if canvas.get("_id_to_control") is Dictionary else {}
	var control: Variant = controls.get(id)
	if control == null or not is_instance_valid(control):
		return McpToolResult.error("'%s' has no live control — is it on the current Screen? (preview_menu show switches screens)." % resource.get_widget_name(id))
	if not control.is_visible_in_tree():
		return McpToolResult.error("'%s' is hidden — show its Window first (a hidden Tab panel's widgets are unclickable)." % resource.get_widget_name(id))
	var fired: Array = resource.get_widget_actions(id)
	if control is BaseButton:
		if control.toggle_mode:
			control.set_pressed(not control.button_pressed)
		else:
			control.emit_signal("pressed")
	elif control.has_method("trigger"):
		control.trigger()
	else:
		return McpToolResult.error("'%s' (%s) is not pressable — press Buttons, Checkboxes, Radios, or Gotos." % [
				resource.get_widget_name(id), String(resource.get_widget_type_name(resource.get_widget_type(id))).to_upper()])
	await ctx.frames(1)
	return {
		"ok": true,
		"pressed": resource.get_widget_name(id),
		"actions": fired,
		"visible_screen": canvas.get_visible_screen_name(),
	}


func _tool_menu_screenshot(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var canvas: Variant = gate["canvas"]
	if canvas == null:
		return McpToolResult.error("The Menus canvas is not mounted — open_in_workspace(workspace=\"mnu\", ...) activates it.")
	var active: Variant = ctx.workspace()
	if active == null or String(active.get_workspace_id()) != "mnu":
		return McpToolResult.error("The Menus workspace is not the active one — open_in_workspace(workspace=\"mnu\", path=<the open menu>) switches to it.")
	if not canvas.is_visible_in_tree():
		return McpToolResult.error("The Menus canvas is not on screen.")
	if args.has("screen") and not (args["screen"] is String):
		return McpToolResult.error("screen must be a name string.")
	if args.has("screen") and not String(args["screen"]).is_empty():
		if canvas.is_interactive():
			return McpToolResult.error("The preview is playing — navigate with preview_menu(op=\"show\") and call menu_screenshot with no screen arg.")
		var err: Error = ws.focus_screen_named(String(args["screen"]))
		if err != OK:
			return McpToolResult.error("No Screen named '%s'. Screens: %s" % [args["screen"], ", ".join(_screen_names(gate["resource"]))])
	await ctx.frames(1)
	var window: Viewport = canvas.get_window()
	if window == null:
		return McpToolResult.error("No window to capture from.")
	var outcome: Dictionary = await McpScreenshot.capture(window, {
		"region_control": canvas,
		"max_dim": int(args.get("max_dim", 1280)),
		"format": String(args.get("format", "webp")),
		"quality": float(args.get("quality", 0.8)),
	})
	if not outcome["ok"]:
		return McpToolResult.error(String(outcome["error"]))
	var caption := "%dx%d %s, %d KiB — workspace=mnu doc=%s screen=%s interactive=%s" % [
		outcome["width"], outcome["height"], outcome["mime"],
		(outcome["bytes"] as PackedByteArray).size() / 1024,
		String(gate["doc"].get("current_path")).get_file(),
		canvas.get_visible_screen_name(), canvas.is_interactive()]
	return McpToolResult.image(outcome["bytes"], outcome["mime"], caption)


# --- analysis / save --------------------------------------------------------------------

func _tool_analyze(args: Dictionary, ctx: McpToolContext) -> Variant:
	var resource: Variant = null
	var source := ""
	if args.has("path") and not String(args["path"]).is_empty():
		resource = NovaMnuDocument.new()
		var resolved := McpAssetDescribe.resolve(ctx, String(args["path"]))
		if not resolved["ok"]:
			return McpToolResult.error(String(resolved["error"]))
		if resource.load_from_bytes(McpAssetDescribe.read_bytes(ctx, resolved)) != OK:
			return McpToolResult.error("Menu failed to parse: %s" % resolved["path"])
		source = String(resolved["path"])
	else:
		var gate := _require_menu(ctx)
		if gate.has("error"):
			return McpToolResult.error(gate["error"])
		resource = gate["resource"]
		var path := String(gate["doc"].get("current_path"))
		source = path if not path.is_empty() else "(untitled tab)"
	var top := clampi(int(args.get("top", 15)), 1, 50)
	var histogram := {}
	var fonts := {}
	var action_graph: Array = []
	var window_wiring := {}
	var cross_files := {}
	var command_hooks := {}
	var id_keys := 0
	var literals := 0
	var text_files := {}
	var widget_total := 0
	var screens: Array = []
	var safety: Array = []
	var all_screen_ids: Array = resource.get_screen_ids()
	for sid in all_screen_ids:
		var screen_name := String(resource.get_screen_name(sid))
		var rsrc := String(resource.get_screen_text_rsrc(sid))
		if not rsrc.is_empty():
			text_files[rsrc] = true
		# Screen-root shape rules, each one a proven original-engine incident:
		# a root without full POSITION + an APPEARANCE row crashed it; "custom"
		# on a covering screen let the main menu's bink video show through.
		var root_id := int(resource.get_screen_root_id(sid))
		var root_apps: Array = resource.get_widget_appearances(root_id)
		if int(resource.get_window_rect_flags(root_id)) != 15:
			safety.append({ "level": "error", "rule": "root_position", "screen": screen_name,
					"detail": "the root window needs a full 4-corner POSITION — the original engine crashes on a screen whose root has none" })
		if root_apps.is_empty():
			safety.append({ "level": "error", "rule": "root_appearance", "screen": screen_name,
					"detail": "the root window needs an APPEARANCE row (type=\"custom\" engine backdrop, or type=\"image\" art like letterbox.tga)" })
		if String(resource.get_widget_name(root_id)) != "MAIN":
			safety.append({ "level": "warn", "rule": "root_name", "screen": screen_name,
					"detail": "every shipped screen root is named MAIN; '%s' is unproven in the original engine" % resource.get_widget_name(root_id) })
		if sid != int(all_screen_ids[0]):
			for app: Dictionary in root_apps:
				if String(app.get("type", "")) == "custom":
					safety.append({ "level": "info", "rule": "custom_backdrop", "screen": screen_name,
							"detail": "type=\"custom\" root appearance is an ENGINE-painted backdrop — whatever plays underneath (e.g. the main menu bink) shows through; covering screens use an image background" })
		var root_frame: Dictionary = resource.get_window_frame(root_id)
		var screen_has_frame: bool = not String(root_frame.get("stencil", "")).is_empty() \
				or not String(root_frame.get("brush", "")).is_empty()
		var stack: Array = [root_id]
		var count := 0
		while not stack.is_empty():
			var id := int(stack.pop_front())
			count += 1
			widget_total += 1
			var type_name := String(resource.get_widget_type_name(resource.get_widget_type(id))).to_upper()
			histogram[type_name] = int(histogram.get(type_name, 0)) + 1
			var font := String(resource.get_widget_font(id))
			if not font.is_empty():
				fonts[font] = int(fonts.get(font, 0)) + 1
			var widget_text := String(resource.get_widget_text(id))
			if not widget_text.is_empty():
				if String(resource.get_widget_string_type(id)) == "id":
					id_keys += 1
				else:
					literals += 1
			var name := String(resource.get_widget_name(id))
			var actions: Array = resource.get_widget_actions(id)
			for action: Dictionary in actions:
				var a_type := String(action.get("type", "")).to_lower()
				if a_type == "screen":
					action_graph.append({ "from": screen_name, "widget": name,
							"to": action.get("target", ""), "file": action.get("file", "") })
					var file := String(action.get("file", ""))
					if not file.is_empty():
						cross_files[file] = true
					else:
						safety.append({ "level": "error", "rule": "screen_action_file", "screen": screen_name, "widget": id, "name": name,
								"detail": "screen actions REQUIRE file= even for same-file jumps (the original engine crashes without it; set_widget_actions auto-fills it)" })
				elif a_type == "window":
					window_wiring[screen_name] = int(window_wiring.get(screen_name, 0)) + 1
				elif a_type.begins_with("pop"):
					action_graph.append({ "from": screen_name, "widget": name, "to": "(pop)" })
			if actions.is_empty() and not name.is_empty():
				var w_type := int(resource.get_widget_type(id))
				if w_type == NovaMnuDocument.TYPE_BUTTON or w_type == NovaMnuDocument.TYPE_CHECKBOX \
						or w_type == NovaMnuDocument.TYPE_RADIO or w_type == NovaMnuDocument.TYPE_GOTO:
					command_hooks[name] = int(command_hooks.get(name, 0)) + 1
			# Widget-shape rules (F4/F5/F8). Each one is calibrated to shipped data:
			# only flag what shipped menus NEVER do, so analyze stays quiet on the
			# game's own files (the false-positive gate). The narrower visual nits
			# from the demo — a label clipped by narrow radio art, art stretched by
			# a width — are NOT here: shipped tabs/toggles do exactly those and
			# render fine, so they live only as advisories in add_menu_widgets.
			if id != root_id:
				var apps: Array = resource.get_widget_appearances(id)
				if PRESSABLE_TYPES.has(type_name) and apps.is_empty():
					safety.append({ "level": "warn", "rule": "pressable_appearance", "screen": screen_name, "widget": id, "name": name,
							"detail": "%s has no APPEARANCE rows — shipped pressables always carry them (empty state rows = text button; image map_state rows = box art)" % type_name })
				if not widget_text.is_empty() and not _has_text_color(resource, id):
					safety.append({ "level": "warn", "rule": "text_color", "screen": screen_name, "widget": id, "name": name,
							"detail": "text with no FG color (own or inherited) renders unreadable dark red in the game — set color default_fg (statics: c4c4c4; pressables: %DEF_TEXT_FG% etc.)" })
				if (int(resource.get_widget_flags(id)) & 8) != 0 and not screen_has_frame:
					safety.append({ "level": "warn", "rule": "draw_frame_assets", "screen": screen_name, "widget": id, "name": name,
							"detail": "DRAW_FRAME draws nothing without FRAME assets on the screen root — set edit_menu_screen props.frame {stencil, brush, monogram}" })
				if type_name == "LIST" and not bool(resource.widget_has_scrollbar(id)):
					safety.append({ "level": "warn", "rule": "list_scrollbar", "screen": screen_name, "widget": id, "name": name,
							"detail": "every shipped LIST carries a SCROLLBAR subtree; one without is unproven in the original engine (not yet authorable here — prefer statics)" })
				if type_name == "SPINLIST" and not bool(resource.widget_has_spin_arrows(id)):
					safety.append({ "level": "warn", "rule": "spinlist_arrows", "screen": screen_name, "widget": id, "name": name,
							"detail": "every shipped SPINLIST carries SPINUP/SPINDOWN arrow structures; one without is unproven in the original engine (not yet authorable here)" })
			for child in resource.get_child_ids(id):
				stack.append(child)
		screens.append({ "name": screen_name, "widgets": count })
	var hooks: Array = []
	for name in command_hooks:
		hooks.append([command_hooks[name], name])
	hooks.sort_custom(func(a, b): return a[0] > b[0])
	var size: Vector2i = resource.get_menu_size()
	var safety_errors := 0
	var safety_warnings := 0
	for finding: Dictionary in safety:
		match String(finding.get("level", "")):
			"error": safety_errors += 1
			"warn": safety_warnings += 1
	return {
		"source": source,
		"menu_size": [size.x, size.y],
		"totals": { "screens": screens.size(), "widgets": widget_total },
		"screens": screens,
		"type_histogram": histogram,
		"action_graph": action_graph.slice(0, 200),
		"window_wiring_per_screen": window_wiring,
		"cross_menu_files": cross_files.keys(),
		"fonts": fonts,
		"strings": { "text_rsrc_files": text_files.keys(), "id_keys": id_keys, "literal_texts": literals },
		"command_hooks": hooks.slice(0, top).map(func(row): return { "name": row[1], "count": row[0] }),
		"game_safety": { "errors": safety_errors, "warnings": safety_warnings,
				"findings": safety.slice(0, 100) },
	}


# True when a text-bearing widget gets a default FG color from itself or any
# ancestor FONT block (%VAR% stylesheet refs count) — without one the original
# engine renders text in an unreadable dark red.
func _has_text_color(resource: Variant, id: int) -> bool:
	var current := id
	while current >= 0 and resource.widget_exists(current) and not resource.is_screen(current):
		if not String(resource.get_widget_color(current, 0)).is_empty():
			return true
		current = resource.get_parent_id(current)
	return false


func _tool_save(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_menu(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var doc: Variant = gate["doc"]
	var path := String(args.get("path", "")).strip_edges()
	var err: Error
	if path.is_empty():
		err = ws.save_current()
		if err == ERR_INVALID_PARAMETER or err == ERR_UNAVAILABLE:
			return McpToolResult.error("This tab is Untitled — pass path (a .mnu filename or absolute path).")
	else:
		if path.get_extension().to_lower() != "mnu":
			return McpToolResult.error("path must end in .mnu.")
		if path.is_relative_path():
			var root_dir := String(ctx.shell.get_resource_root_dir()) if ctx.shell != null and ctx.shell.has_method("get_resource_root_dir") else ""
			if root_dir.is_empty():
				return McpToolResult.error("No resource root mounted to resolve a relative filename — pass an absolute path.")
			path = root_dir.path_join(path)
		err = doc.save_as_path(path)
	if err != OK:
		return McpToolResult.error("Save failed (%s)." % error_string(err))
	return { "ok": true, "path": doc.get("current_path"), "dirty": doc.get("is_dirty"), "documents": _tabs_block(ws) }
