extends GutTest

# The menu-authoring MCP tools, end-to-end on the real Menus workspace
# (mounted editor + canvas, headless): the headline loop builds a 2-screen
# menu with a navigating button and CLICKS through it in the Interactive
# preview; the rest covers batch-undo atomicity, validation vocabularies,
# %VAR% round-trips, items/tables, multidoc tabs, and save.

const MnuWorkspaceScript := preload("res://modtools/mnu/mnu_workspace.gd")

const ALL_WIDGETS := "res://../fixtures/mnu/all_widgets.mnu"
const JO_OPTIONS := "res://../fixtures/mnu/jo_options.mnu"
const MENU_STYLE := "res://../fixtures/mns/menu_style.mns"
const SAVE_DIR := "user://mcp_menu_tools_test"

var ws: RefCounted
var mount: Control
var service: EditorMcpService
var shell: Node


class ShellStub:
	extends Node

	var _workspaces := {}
	var root_dir := ""

	func get_resource_root_dir() -> String:
		return root_dir

	func get_resource_root() -> NovaResourceRoot:
		return null

	func _get_active_workspace() -> Variant:
		return _workspaces.values()[0] if not _workspaces.is_empty() else null


func _abs(p: String) -> String:
	return ProjectSettings.globalize_path(p)


func before_each() -> void:
	if FileAccess.file_exists(MnuWorkspaceScript.STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(MnuWorkspaceScript.STATE_PATH))
	ws = MnuWorkspaceScript.new()
	mount = add_child_autofree(Control.new())
	mount.size = Vector2(800, 600)
	ws.mount_viewport(mount)
	await get_tree().process_frame
	shell = add_child_autofree(ShellStub.new())
	shell._workspaces = { 0: ws }
	shell.root_dir = _abs(SAVE_DIR)
	DirAccess.make_dir_recursive_absolute(_abs(SAVE_DIR))
	service = add_child_autofree(EditorMcpService.new())
	service.setup(add_child_autofree(Node.new()), shell)


func after_each() -> void:
	service.stop()
	McpLogHub.instance = null
	if ws != null:
		ws.release_viewport()
		ws = null
	if FileAccess.file_exists(MnuWorkspaceScript.STATE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(MnuWorkspaceScript.STATE_PATH))
	var dir := _abs(SAVE_DIR)
	if DirAccess.dir_exists_absolute(dir):
		var d := DirAccess.open(dir)
		if d != null:
			for f in d.get_files():
				DirAccess.remove_absolute(dir.path_join(f))
		DirAccess.remove_absolute(dir)


func _call(name: String, args := {}) -> McpToolResult:
	var ctx: McpToolContext = service._make_context(args)
	return await service.server.registry.call_tool(name, args, ctx)


func _resource() -> NovaMnuDocument:
	return ws.get_menu_resource()


func test_headline_authoring_and_preview_loop() -> void:
	# Build: a second screen, a button on the first screen that navigates to it.
	var added_screen: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "SECOND" } })
	assert_false(added_screen.is_error, str(added_screen.content))
	var first_screen := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first_screen, "type": "BUTTON", "rect": [200, 200, 120, 32], "name": "GotoSecond", "text": "Go" },
	] })
	assert_false(added.is_error, str(added.content))
	var button_id := int(added.structured["ids"][0])
	var wired: McpToolResult = await _call("set_widget_actions", { "id": button_id,
			"actions": [{ "type": "screen", "target": "SECOND" }] })
	assert_false(wired.is_error, str(wired.content))
	# This menu is Untitled, so the same-file action wires with an empty file= and
	# a single "save before shipping" warning (the preview still navigates it).
	var warns: Array = wired.structured["warnings"]
	assert_true(warns.is_empty() or String(warns[0]).contains("Untitled"),
			"Only the expected Untitled file= note, if any: %s" % [warns])

	# Read back: the tree carries the button with its action.
	var menu: McpToolResult = await _call("get_menu", { "detail": "full" })
	assert_false(menu.is_error)
	assert_eq((menu.structured["screens"] as Array).size(), 2)
	var card: McpToolResult = await _call("get_menu", { "widget": button_id })
	assert_eq(String(card.structured["actions"][0]["target"]), "SECOND")

	# Play: press the button, land on SECOND, pop back.
	var on: McpToolResult = await _call("preview_menu", { "op": "on" })
	assert_false(on.is_error, str(on.content))
	var live_state: MnuPreviewWidgetState = \
		ws.get_editor_document().get_preview_widget_state(button_id)
	assert_true(live_state.visible,
		"new button is live on the starting Screen")
	var pressed: McpToolResult = await _call("preview_menu", { "op": "press", "widget": "GotoSecond" })
	assert_false(pressed.is_error, str(pressed.content))
	assert_eq(String(pressed.structured["visible_screen"]), "SECOND", "Pressing the button navigates.")
	var back: McpToolResult = await _call("preview_menu", { "op": "back" })
	assert_true(bool(back.structured["popped"]))
	assert_eq(String(back.structured["visible_screen"]), first_screen)
	await _call("preview_menu", { "op": "off" })

	# Study: the analyzer sees the edge and the histogram.
	var analyzed: McpToolResult = await _call("analyze_menu")
	assert_false(analyzed.is_error)
	assert_eq(int(analyzed.structured["type_histogram"]["BUTTON"]), 1)
	var graph: Array = analyzed.structured["action_graph"]
	assert_eq(String(graph[0]["to"]), "SECOND")


func test_add_widgets_batch_is_atomic_and_validates() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var bogus_type: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "NOT_A_TYPE", "rect": [0, 0, 10, 10] },
	] })
	assert_true(bogus_type.is_error)
	assert_true(String(bogus_type.content[0]["text"]).contains("BUTTON"), "Error lists the type vocabulary.")
	var bogus_parent: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "BUTTON", "rect": [0, 0, 10, 10] },
		{ "parent": 987654, "type": "BUTTON", "rect": [0, 0, 10, 10] },
	] })
	assert_true(bogus_parent.is_error, "An unknown parent rejects the whole batch.")
	assert_eq(int((await _call("get_menu")).structured["screens"][0]["tree"]["children"].size() if (await _call("get_menu")).structured["screens"][0]["tree"].has("children") else 0), 0,
			"No partial adds happened.")

	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "BUTTON", "rect": [10, 10, 100, 24], "name": "B1", "text": "One" },
		{ "parent": first, "type": "STATIC", "rect": [10, 40, 100, 24], "name": "S1" },
		{ "parent": first, "type": "LABEL", "rect": [10, 70, 100, 24], "name": "L1" },
		{ "parent": first, "type": "CHECKBOX", "rect": [10, 100, 100, 24], "name": "C1" },
		{ "parent": first, "type": "LIST", "rect": [10, 130, 100, 80], "name": "List1" },
	] })
	assert_false(added.is_error)
	assert_eq(int(added.structured["added"]), 5)
	assert_eq(int(added.structured["undo_steps"]), 1)
	var undone: McpToolResult = await _call("undo", { "workspace": "mnu" })
	assert_eq(int(undone.structured["performed"]), 1)
	for id in added.structured["ids"]:
		assert_false(_resource().widget_exists(int(id)), "ONE undo removed the whole batch.")


func test_edit_widget_props_var_roundtrip_and_guards() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "BUTTON", "rect": [10, 10, 100, 24], "name": "Styled" },
		{ "parent": first, "type": "WINDOW", "rect": [150, 10, 200, 200], "name": "Panel" },
	] })
	var button_id := int(added.structured["ids"][0])
	var panel_id := int(added.structured["ids"][1])

	var styled: McpToolResult = await _call("edit_menu_widget", { "set": { "id": button_id,
			"props": { "color": { "slot": "default_fg", "value": "%TITLE_COLOR%" }, "text": "Hello" } } })
	assert_false(styled.is_error, str(styled.content))
	assert_eq(String(styled.structured["widget"]["colors"]["default_fg"]), "%TITLE_COLOR%",
			"Stylesheet references round-trip verbatim.")

	var bogus: McpToolResult = await _call("edit_menu_widget", { "set": { "id": button_id, "props": { "nope": 1 } } })
	assert_true(bogus.is_error)
	assert_true(String(bogus.content[0]["text"]).contains("string_type"), "Error lists the prop vocabulary.")
	var act: McpToolResult = await _call("edit_menu_widget", { "set": { "id": button_id, "props": { "actions": [] } } })
	assert_true(act.is_error)
	assert_true(String(act.content[0]["text"]).contains("set_widget_actions"))

	var moved: McpToolResult = await _call("edit_menu_widget", { "move_rects": { "rows": [
		{ "id": button_id, "rect": [20, 20, 100, 24] },
		{ "id": panel_id, "rect": [160, 20, 200, 200] },
	] } })
	assert_false(moved.is_error)
	assert_eq(int(moved.structured["undo_steps"]), 1)

	var reparented: McpToolResult = await _call("edit_menu_widget", { "reparent": { "id": button_id, "parent": panel_id, "index": 0 } })
	assert_false(reparented.is_error, str(reparented.content))
	assert_eq(int(_resource().get_parent_id(button_id)), panel_id)

	var root := _resource().get_screen_root_id(_resource().get_screen_ids()[0])
	var root_delete: McpToolResult = await _call("edit_menu_widget", { "delete": root })
	assert_true(root_delete.is_error, "Root windows are not deletable.")
	var deleted: McpToolResult = await _call("edit_menu_widget", { "delete": button_id })
	assert_false(deleted.is_error)
	assert_false(_resource().widget_exists(button_id))


func test_action_validation_and_sounds() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "BUTTON", "rect": [10, 10, 100, 24], "name": "Nav" },
	] })
	var id := int(added.structured["ids"][0])
	var bogus: McpToolResult = await _call("set_widget_actions", { "id": id,
			"actions": [{ "type": "screen", "target": "NOWHERE" }] })
	assert_true(bogus.is_error)
	assert_true(String(bogus.content[0]["text"]).contains(first), "Error lists the real Screens.")
	var warned: McpToolResult = await _call("set_widget_actions", { "id": id,
			"actions": [{ "type": "window", "target": "NoSuchPanel", "state": "SHOW" }] })
	assert_false(warned.is_error)
	assert_gt((warned.structured["warnings"] as Array).size(), 0, "Unmatched window targets warn.")
	var sounds: McpToolResult = await _call("set_widget_actions", { "id": id,
			"sounds": [{ "state": "MOUSEOVER", "trigger": "MOUSE_OVER", "file": "menu.lwf" }] })
	assert_false(sounds.is_error)
	assert_eq(String(sounds.structured["sounds"][0]["trigger"]), "MOUSE_OVER")


func test_all_action_types_and_fields_cross_the_mcp_seam() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", {"rows": [
		{"parent": first, "type": "BUTTON", "rect": [0, 0, 80, 24], "name": "Actions"},
	]})
	var id := int(added.structured["ids"][0])
	var types := [
		"screen", "window", "url", "form_post",
		"glb_load", "glb_loadandping", "glb_filter", "glb_filter_num",
		"glb_ping", "glb_join", "tab", "pop_screen", "appmsg",
		"lan_search", "lan_join", "mnx",
	]
	var actions: Array = []
	for type in types:
		var row := {
			"type": type, "state": "", "file": "", "source": "profile",
			"field": "callsign", "test": "EQ", "target": "payload",
			"has_target_form": true, "target_form": 0,
			"external_browser": type == "url", "toggle": true,
		}
		if type == "screen":
			row["target"] = first
		elif type == "window":
			row["target"] = "Actions"
			row["state"] = "SHOW"
		actions.append(row)
	var wired: McpToolResult = await _call("set_widget_actions",
		{"id": id, "actions": actions})
	assert_false(wired.is_error, str(wired.content))
	var saved: Array = wired.structured["actions"]
	assert_eq(saved.size(), 16)
	for i in range(types.size()):
		assert_eq(String(saved[i]["type"]), types[i])
	assert_eq(String(saved[-1]["source"]), "profile")
	assert_eq(String(saved[-1]["field"]), "callsign")
	assert_eq(String(saved[-1]["test"]), "EQ")
	assert_true(bool(saved[-1]["has_target_form"]))
	assert_eq(int(saved[-1]["target_form"]), 0)
	assert_true(bool(saved[-1]["toggle"]))
	assert_true(bool(saved[2]["external_browser"]))


func test_items_and_table_ops() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "LIST", "rect": [10, 10, 200, 120], "name": "Choices" },
		{ "parent": first, "type": "TABLE", "rect": [10, 150, 300, 120], "name": "Grid" },
	] })
	var list_id := int(added.structured["ids"][0])
	var table_id := int(added.structured["ids"][1])

	var add_a: McpToolResult = await _call("edit_widget_items", { "id": list_id, "op": "item_add", "row": { "text": "Alpha" } })
	assert_false(add_a.is_error, str(add_a.content))
	await _call("edit_widget_items", { "id": list_id, "op": "item_add", "row": { "text": "Bravo" } })
	var renamed: McpToolResult = await _call("edit_widget_items", { "id": list_id, "op": "item_field", "index": 0, "key": "text", "value": "Alpha2" })
	assert_eq(String(renamed.structured["items"][0]["text"]), "Alpha2")
	var swapped: McpToolResult = await _call("edit_widget_items", { "id": list_id, "op": "item_move", "from": 0, "to": 1 })
	assert_eq(String(swapped.structured["items"][1]["text"]), "Alpha2")
	var out_of_range: McpToolResult = await _call("edit_widget_items", { "id": list_id, "op": "item_remove", "index": 9 })
	assert_true(out_of_range.is_error)
	var removed: McpToolResult = await _call("edit_widget_items", { "id": list_id, "op": "item_remove", "index": 0 })
	assert_eq((removed.structured["items"] as Array).size(), 1)

	var sized: McpToolResult = await _call("edit_widget_items", { "id": table_id, "op": "set_table", "count": 3, "spacing": 4 })
	assert_eq(int(sized.structured["count"]), 3)
	var header: McpToolResult = await _call("edit_widget_items", { "id": table_id, "op": "header_add",
			"row": { "column": 0, "width": 80, "text": "Name" } })
	assert_false(header.is_error, str(header.content))
	assert_eq((header.structured["headers"] as Array).size(), 1)


func test_deep_authoring_supports_glb_table_lan_list_and_both_combo_items() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", {"rows": [
		{"parent": first, "type": "GLB_TABLE", "rect": [10, 10, 300, 100], "name": "GlobalGrid"},
		{"parent": first, "type": "LAN_LIST", "rect": [10, 120, 300, 100], "name": "LanRows"},
		{"parent": first, "type": "COMBOBOX", "rect": [10, 230, 200, 30], "name": "Mode"},
	]})
	assert_false(added.is_error, str(added.content))
	var grid := int(added.structured["ids"][0])
	var lan := int(added.structured["ids"][1])
	var combo := int(added.structured["ids"][2])
	var grid_edit: McpToolResult = await _call("edit_menu_widget", {"set": {
		"id": grid, "props": {"authoring": {
			"items": {"present": true, "appearances": [
				{"state": "default", "type": "outline", "value": "112233"},
				{"state": "selected", "type": "color", "value": "445566"},
			]},
			"table": {
				"has_count": true, "count": 2, "has_spacing": true, "spacing": 3,
				"headers": [{"has_column": true, "column": 0,
					"has_width": true, "width": 120, "text": "Server"}],
			},
		}},
	}})
	assert_false(grid_edit.is_error, str(grid_edit.content))
	var lan_edit: McpToolResult = await _call("edit_menu_widget", {"set": {
		"id": lan, "props": {"authoring": {"items": {
			"present": true, "rows": [{"type": "", "value": "1", "text": "LAN"}],
		}}},
	}})
	assert_false(lan_edit.is_error, str(lan_edit.content))
	var combo_edit: McpToolResult = await _call("edit_menu_widget", {"set": {
		"id": combo, "props": {"authoring": {
			"items": {"present": true,
				"rows": [{"type": "", "value": "closed", "text": "Closed"}]},
			"list_box": {"present": true, "items": {"present": true,
				"rows": [{"type": "", "value": "popup", "text": "Popup"}]}},
		}},
	}})
	assert_false(combo_edit.is_error, str(combo_edit.content))

	var grid_card: McpToolResult = await _call("get_menu", {"widget": grid})
	assert_eq(int(grid_card.structured["authoring"]["table"]["count"]), 2)
	assert_eq(String(grid_card.structured["authoring"]["table"]["headers"][0]["text"]),
		"Server")
	var lan_card: McpToolResult = await _call("get_menu", {"widget": lan})
	assert_eq(String(lan_card.structured["authoring"]["items"]["rows"][0]["text"]), "LAN")
	var combo_card: McpToolResult = await _call("get_menu", {"widget": combo})
	assert_eq(String(combo_card.structured["authoring"]["items"]["rows"][0]["text"]),
		"Closed")
	assert_eq(String(combo_card.structured["authoring"]["list_box"]["items"]["rows"][0]["text"]),
		"Popup")
	var glb_item: McpToolResult = await _call("edit_widget_items",
		{"id": grid, "op": "item_add", "row": {"text": "Row"}})
	assert_false(glb_item.is_error, "GLB_TABLE supports item rows")
	var lan_item: McpToolResult = await _call("edit_widget_items",
		{"id": lan, "op": "item_add", "row": {"text": "LAN two"}})
	assert_false(lan_item.is_error, "LAN_LIST supports item rows")


func test_authoring_validation_rejects_unknown_nested_keys_without_partial_edit() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", {"rows": [
		{"parent": first, "type": "BUTTON", "rect": [0, 0, 80, 24], "name": "Atomic"},
	]})
	var id := int(added.structured["ids"][0])
	var editor: Object = ws.get_editor_document()
	editor.restore_history({})
	for malformed in [
		{"name": "PartiallyMutated", "authoring": {"items": {"rowz": []}}},
		{"authoring": {"table": {"spcaing": 4}}},
		{"authoring": {"list_box": {"items": {"rows": ["not an object"]}}}},
		{"authoring": {"hotkeys": "not an array"}},
	]:
		var refused: McpToolResult = await _call("edit_menu_widget",
			{"set": {"id": id, "props": malformed}})
		assert_true(refused.is_error, "malformed deep patches fail")
		assert_eq(String(_resource().get_widget_name(id)), "Atomic",
			"validation happens before the first scalar mutation")
		assert_false(editor.can_undo(),
			"rejected patches create no undo entry")
	var noop: McpToolResult = await _call("edit_menu_widget", {"set": {
		"id": id, "props": {"authoring": {"name": "Atomic"}},
	}})
	assert_false(noop.is_error)
	assert_false(editor.can_undo(),
		"idempotent authoring patches are strict no-ops")


func test_unsupported_item_ops_and_atomic_table_sizing() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", {"rows": [
		{"parent": first, "type": "BUTTON", "rect": [0, 0, 80, 24], "name": "Plain"},
		{"parent": first, "type": "TABLE", "rect": [0, 40, 200, 100], "name": "Grid"},
	]})
	var button := int(added.structured["ids"][0])
	var table := int(added.structured["ids"][1])
	var refused: McpToolResult = await _call("edit_widget_items",
		{"id": button, "op": "item_add", "row": {"text": "Nope"}})
	assert_true(refused.is_error, "BUTTON does not silently accept ITEMS")
	var editor: Object = ws.get_editor_document()
	editor.restore_history({})
	var sized: McpToolResult = await _call("edit_widget_items",
		{"id": table, "op": "set_table", "count": 4, "spacing": 7})
	assert_false(sized.is_error, str(sized.content))
	assert_true(editor.can_undo(),
		"count plus spacing share one snapshot undo")
	editor.undo()
	assert_false(editor.can_undo(), "the table edit contributed exactly one undo entry")
	var table_state: Dictionary = _resource().get_widget_authoring_state(table)["table"]
	assert_false(bool(table_state["has_count"]))
	assert_false(bool(table_state["has_spacing"]))
	assert_eq(int(table_state["count"]), 0)
	assert_eq(int(table_state["spacing"]), 0)


func test_screen_ops_and_rules() -> void:
	var last_delete: McpToolResult = await _call("edit_menu_screen", { "delete": String(_resource().get_screen_name(_resource().get_screen_ids()[0])) })
	assert_true(last_delete.is_error, "The last Screen is protected.")
	await _call("edit_menu_screen", { "add": { "name": "OPTIONS" } })
	var dup: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "OPTIONS" } })
	assert_true(dup.is_error, "Duplicate Screen names are refused.")
	var shown: McpToolResult = await _call("edit_menu_screen", { "show": "OPTIONS" })
	assert_eq(String(shown.structured["visible_screen"]), "OPTIONS")
	var filtered: McpToolResult = await _call("get_menu", {"screen": "options"})
	assert_false(filtered.is_error, "Screen lookup is case-insensitive")
	assert_eq(String(filtered.structured["screens"][0]["name"]), "OPTIONS")
	var renamed: McpToolResult = await _call("edit_menu_screen", { "set": { "screen": "OPTIONS", "props": { "name": "SETTINGS", "music_var": 3 } } })
	assert_false(renamed.is_error, str(renamed.content))
	var sid := _resource().get_screen_ids()[1]
	assert_eq(String(_resource().get_screen_name(sid)), "SETTINGS")
	assert_eq(int(_resource().get_screen_music_var(sid)), 3)
	var deleted: McpToolResult = await _call("edit_menu_screen", { "delete": "SETTINGS" })
	assert_false(deleted.is_error)
	assert_eq(_resource().get_screen_count(), 1)


func test_interactive_preview_locks_editing() -> void:
	var on: McpToolResult = await _call("preview_menu", { "op": "on" })
	assert_true(ws.get_editor_document().is_interactive(),
		"preview_menu uses the editor-level interaction lock")
	assert_true(bool(on.structured["interactive"]))
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	for spec in [
		["add_menu_widgets", { "rows": [{ "parent": first, "type": "BUTTON", "rect": [0, 0, 10, 10] }] }],
		["edit_menu_widget", { "delete": 1 }],
		["edit_menu_screen", { "add": { "name": "X" } }],
		["set_widget_actions", { "id": 1, "actions": [] }],
		["edit_widget_items", { "id": 1, "op": "item_add", "row": {} }],
		["menu_tabs", { "op": "new" }],
	]:
		var result: McpToolResult = await _call(spec[0], spec[1])
		assert_true(result.is_error, "%s is locked while the preview plays" % spec[0])
		assert_true(String(result.content[0]["text"]).contains("preview_menu"), "%s names the fixing tool" % spec[0])
	var off: McpToolResult = await _call("preview_menu", { "op": "off" })
	assert_false(ws.get_editor_document().is_interactive())
	assert_false(bool(off.structured["interactive"]))
	var unlocked: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "X" } })
	assert_false(unlocked.is_error, "off unlocks editing")


func test_preview_radio_dispatches_actions_and_disabled_consumes_without_activation() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", {"rows": [
		{"parent": first, "type": "WINDOW", "rect": [200, 40, 120, 80],
			"name": "Panel", "flags": NovaMnuDocument.FLAG_HIDDEN},
		{"parent": first, "type": "RADIO", "rect": [20, 40, 24, 24],
			"name": "RadioShow", "group": 1, "appearances": [
				{"state": "default"}, {"state": "selected"},
			]},
	]})
	assert_false(added.is_error, str(added.content))
	var panel := int(added.structured["ids"][0])
	var radio := int(added.structured["ids"][1])
	var wired: McpToolResult = await _call("set_widget_actions", {"id": radio,
		"actions": [{"type": "window", "state": "SHOW", "target": "Panel"}]})
	assert_false(wired.is_error, str(wired.content))
	await _call("preview_menu", {"op": "on"})
	var pressed: McpToolResult = await _call("preview_menu",
		{"op": "press", "widget": "RadioShow"})
	assert_false(pressed.is_error, str(pressed.content))
	assert_eq(String(pressed.structured["actions"][0]["type"]), "window",
		"RADIO activation reports and dispatches its authored action")
	var radio_state: MnuPreviewWidgetState = \
		ws.get_editor_document().get_preview_widget_state(radio)
	var panel_state: MnuPreviewWidgetState = \
		ws.get_editor_document().get_preview_widget_state(panel)
	assert_true(radio_state.pressed,
		"radio toggles before pressed is dispatched")
	assert_true(panel_state.visible,
		"the Window SHOW action reached the live menu")
	await _call("preview_menu", {"op": "off"})

	await _call("edit_menu_widget", {"set": {"id": panel,
		"props": {"flags": NovaMnuDocument.FLAG_HIDDEN}}})
	await _call("edit_menu_widget", {"set": {"id": radio,
		"props": {"flags": NovaMnuDocument.FLAG_DISABLED}}})
	await _call("preview_menu", {"op": "on"})
	var disabled: McpToolResult = await _call("preview_menu",
		{"op": "press", "widget": radio})
	assert_false(disabled.is_error, "a disabled visible target consumes the press")
	assert_true(bool(disabled.structured["disabled"]))
	assert_false(bool(disabled.structured["activated"]))
	panel_state = ws.get_editor_document().get_preview_widget_state(panel)
	assert_false(panel_state.visible,
		"disabled radio dispatches no SHOW action")
	await _call("preview_menu", {"op": "off"})


func test_menu_tabs_and_per_tab_undo_isolation() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	await _call("add_menu_widgets", { "rows": [{ "parent": first, "type": "BUTTON", "rect": [0, 0, 10, 10] }] })
	var created: McpToolResult = await _call("menu_tabs", { "op": "new" })
	assert_false(created.is_error, str(created.content))
	assert_eq((created.structured["documents"]["tabs"] as Array).size(), 2)
	# Tab 1 is fresh: its history is empty even though tab 0 has an edit.
	var undone: McpToolResult = await _call("undo", { "workspace": "mnu" })
	assert_eq(int(undone.structured["performed"]), 0, "Per-tab histories: the new tab has nothing to undo.")
	var dirty_close: McpToolResult = await _call("menu_tabs", { "op": "close", "index": 0 })
	assert_true(dirty_close.is_error, "A dirty tab refuses to close without discard.")
	var discarded: McpToolResult = await _call("menu_tabs", { "op": "close", "index": 0, "discard": true })
	assert_false(discarded.is_error)
	assert_eq((discarded.structured["documents"]["tabs"] as Array).size(), 1)


func test_save_menu_round_trip_and_errors() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	await _call("add_menu_widgets", { "rows": [{ "parent": first, "type": "BUTTON", "rect": [0, 0, 10, 10], "name": "Saved" }] })
	var untitled: McpToolResult = await _call("save_menu")
	assert_true(untitled.is_error, "Untitled without a path is an actionable error.")
	var bad_ext: McpToolResult = await _call("save_menu", { "path": "thing.txt" })
	assert_true(bad_ext.is_error)
	var saved: McpToolResult = await _call("save_menu", { "path": "authored.mnu" })
	assert_false(saved.is_error, str(saved.content))
	var path := String(saved.structured["path"])
	assert_true(path.begins_with(_abs(SAVE_DIR)), "Relative filenames land in the mounted root.")
	assert_false(bool(saved.structured["dirty"]))
	var reloaded := NovaMnuDocument.new()
	assert_eq(reloaded.load_from_bytes(FileAccess.get_file_as_bytes(path)), OK)
	assert_gt(reloaded.get_screen_count(), 0, "The saved menu round-trips.")
	var resaved: McpToolResult = await _call("save_menu")
	assert_false(resaved.is_error, "save_current works once a path exists.")


func test_analyze_menu_path_mode_on_shipped_fixture() -> void:
	var analyzed: McpToolResult = await _call("analyze_menu", { "path": _abs(JO_OPTIONS) })
	assert_false(analyzed.is_error, str(analyzed.content))
	var out: Dictionary = analyzed.structured
	assert_gt(int(out["totals"]["widgets"]), 10)
	assert_gt((out["type_histogram"] as Dictionary).size(), 2)
	assert_true(out.has("action_graph"))
	assert_true(out.has("command_hooks"))
	var bogus: McpToolResult = await _call("analyze_menu", { "path": "no_such_menu.mnu" })
	assert_true(bogus.is_error)


func test_preview_press_error_paths() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "STATIC", "rect": [10, 10, 50, 20], "name": "JustText" },
		{ "parent": first, "type": "BUTTON", "rect": [10, 40, 50, 20], "name": "Hid", "flags": 1 },
	] })
	var static_id := int(added.structured["ids"][0])
	var not_playing: McpToolResult = await _call("preview_menu", { "op": "press", "widget": static_id })
	assert_true(not_playing.is_error)
	await _call("preview_menu", { "op": "on" })
	var not_pressable: McpToolResult = await _call("preview_menu", { "op": "press", "widget": static_id })
	assert_true(not_pressable.is_error)
	assert_true(String(not_pressable.content[0]["text"]).contains("not pressable"))
	var hidden: McpToolResult = await _call("preview_menu", { "op": "press", "widget": "Hid" })
	assert_true(hidden.is_error, "Hidden widgets are unclickable, like for a human.")
	await _call("preview_menu", { "op": "off" })


func test_menu_screenshot_headless_guard_chain() -> void:
	var unknown: McpToolResult = await _call("menu_screenshot", { "screen": "NOWHERE" })
	assert_true(unknown.is_error)
	var shot: McpToolResult = await _call("menu_screenshot")
	assert_true(shot.is_error, "Headless capture fails through the guard chain, never hangs.")


func test_get_menu_on_all_widgets_fixture() -> void:
	assert_eq(int(ws.open_file(_abs(ALL_WIDGETS))), OK)
	await get_tree().process_frame
	var menu: McpToolResult = await _call("get_menu", { "detail": "full", "max_widgets": 2000 })
	assert_false(menu.is_error, str(menu.content))
	assert_gt((menu.structured["screens"] as Array).size(), 0)
	var first_tree: Dictionary = menu.structured["screens"][0]["tree"]
	assert_true(first_tree.has("children"), "The fixture tree is populated.")


func test_menu_tools_stay_on_the_menu_editor_after_opening_styles() -> void:
	assert_eq(int(ws.open_file(_abs(ALL_WIDGETS))), OK)
	assert_eq(int(ws.open_file(_abs(MENU_STYLE))), OK)
	await get_tree().process_frame
	var menu: McpToolResult = await _call("get_menu")
	assert_false(menu.is_error, str(menu.content))
	assert_eq(String(menu.structured["path"]), _abs(ALL_WIDGETS))
	assert_gt((menu.structured["screens"] as Array).size(), 0)
	assert_ne(ws.get_editor_document(), ws.get_menu_editor(),
		"the successful MCP query really ran while the Styles editor was active")

	assert_eq(int(ws.open_file(_abs(ALL_WIDGETS))), OK)
	assert_eq(ws.get_editor_document(), ws.get_menu_editor(),
		"opening an existing MNU tab returns shell undo and commands to the menu editor")


func test_table_ops_refuse_non_table_widgets() -> void:
	# header_*/body_*/subst_*/set_table are TABLE-column ops; the document
	# silently ignores them on other widget types, so the tool must refuse.
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "SPINLIST", "rect": [10, 10, 200, 20], "name": "Spin" },
	] })
	var spin_id := int(added.structured["ids"][0])
	for op in ["header_add", "body_add", "subst_add"]:
		var refused: McpToolResult = await _call("edit_widget_items", { "id": spin_id, "op": op, "row": { "text": "x" } })
		assert_true(refused.is_error, "%s on a SPINLIST is an error" % op)
		assert_true(String(refused.content[0]["text"]).contains("TABLE"), "%s names the type rule" % op)
		assert_true(String(refused.content[0]["text"]).contains("item_add"), "%s points at the item_* family" % op)
	var sized: McpToolResult = await _call("edit_widget_items", { "id": spin_id, "op": "set_table", "count": 3 })
	assert_true(sized.is_error, "set_table on a SPINLIST is an error")
	var item: McpToolResult = await _call("edit_widget_items", { "id": spin_id, "op": "item_add", "row": { "text": "Low" } })
	assert_false(item.is_error, "item_* stays the right verb for SPINLIST rows")
	assert_eq((item.structured["items"] as Array).size(), 1)


func test_malformed_arg_shapes_error_instead_of_silently_passing() -> void:
	# Wrapping a screen ref in an object used to come back ok:true while doing
	# nothing; every op now rejects wrong-shaped args with an actionable error.
	var shown: McpToolResult = await _call("edit_menu_screen", { "show": { "screen": "STARTUP" } })
	assert_true(shown.is_error, "show with an object ref errors")
	var deleted: McpToolResult = await _call("edit_menu_screen", { "delete": { "screen": "STARTUP" } })
	assert_true(deleted.is_error, "delete with an object ref errors")
	var set_str: McpToolResult = await _call("edit_menu_screen", { "set": "STARTUP" })
	assert_true(set_str.is_error, "set with a bare string errors")
	var add_str: McpToolResult = await _call("edit_menu_screen", { "add": "X" })
	assert_true(add_str.is_error, "add with a bare string errors")
	var widget_del: McpToolResult = await _call("edit_menu_widget", { "delete": { "id": 3 } })
	assert_true(widget_del.is_error, "widget delete with an object errors")
	var widget_set: McpToolResult = await _call("edit_menu_widget", { "set": "name" })
	assert_true(widget_set.is_error, "widget set with a bare string errors")
	var moved: McpToolResult = await _call("edit_menu_widget", { "move_rects": { "rows": ["x"] } })
	assert_true(moved.is_error, "non-object move_rects rows error")
	await _call("preview_menu", { "op": "on" })
	var bad_show: McpToolResult = await _call("preview_menu", { "op": "show", "screen": { "name": "X" } })
	assert_true(bad_show.is_error, "preview show with an object screen errors")
	var bad_press: McpToolResult = await _call("preview_menu", { "op": "press", "widget": [1] })
	assert_true(bad_press.is_error, "press with an array widget ref errors")
	await _call("preview_menu", { "op": "off" })


func test_screen_add_is_game_shaped_with_background_and_copies_props() -> void:
	# Give screen 1 the shipped per-screen props, then add a second screen and
	# expect them copied plus a game-shaped root with an image backdrop + frame.
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var seeded: McpToolResult = await _call("edit_menu_screen", { "set": { "screen": first, "props": {
		"music_var": 7, "text_rsrc": "menutxt.BIN", "cursor_file": "newarow1.tga" } } })
	assert_false(seeded.is_error, str(seeded.content))
	var added: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "SHAPED",
			"background": "letterbox.tga",
			"frame": { "stencil": "BORDER2.tga", "stencil_size": 32, "brush": "BOXTILE.tga" } } })
	assert_false(added.is_error, str(added.content))
	var sid := int(added.structured["screen_id"])
	var root_id := int(added.structured["root_id"])
	assert_eq(int(_resource().get_screen_music_var(sid)), 7, "music_var copied from screen 1")
	assert_eq(String(_resource().get_screen_text_rsrc(sid)), "menutxt.BIN", "text_rsrc copied")
	assert_eq(String(_resource().get_screen_cursor_file(sid)), "newarow1.tga", "cursor copied")
	assert_eq(String(_resource().get_widget_name(root_id)), "MAIN")
	var apps: Array = _resource().get_widget_appearances(root_id)
	assert_eq(apps.size(), 1)
	assert_eq(String(apps[0]["type"]), "image", "background swaps the custom row for an image backdrop")
	assert_eq(String(apps[0]["value"]), "letterbox.tga")
	assert_eq(String(_resource().get_window_frame(root_id)["brush"]), "BOXTILE.tga")
	# The widget card surfaces the new structure for copying.
	var card: McpToolResult = await _call("get_menu", { "widget": root_id })
	assert_true((card.structured["appearances"] as Array).size() == 1)
	assert_true(card.structured.has("frame"))


func test_screen_actions_auto_fill_their_own_file() -> void:
	# The game requires file= on every screen action (same-file jumps name their
	# own file — mp.mnu's proven pattern); Untitled documents must save first.
	var added_screen: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "SECOND" } })
	assert_false(added_screen.is_error)
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "BUTTON", "rect": [10, 10, 100, 25], "name": "Jump" },
	] })
	var btn := int(added.structured["ids"][0])

	# Untitled: the action is allowed (the in-editor preview navigates same-file)
	# but file= stays empty with a warning — the game needs it before shipping.
	var untitled: McpToolResult = await _call("set_widget_actions", { "id": btn,
			"actions": [{ "type": "screen", "target": "SECOND" }] })
	assert_false(untitled.is_error, str(untitled.content))
	assert_eq(String((untitled.structured["actions"] as Array)[0]["file"]), "", "Untitled leaves file empty")
	assert_true(String("\n".join(PackedStringArray(untitled.structured["warnings"]))).contains("Untitled"))

	# Saved: re-wiring auto-fills the action with the menu's own filename.
	var saved: McpToolResult = await _call("save_menu", { "path": _abs(SAVE_DIR).path_join("autofill.mnu") })
	assert_false(saved.is_error, str(saved.content))
	var wired: McpToolResult = await _call("set_widget_actions", { "id": btn,
			"actions": [{ "type": "SCREEN", "target": "SECOND" }] })
	assert_false(wired.is_error, str(wired.content))
	var action: Dictionary = (wired.structured["actions"] as Array)[0]
	assert_eq(String(action["file"]), "autofill.mnu", "file auto-filled with the menu's own name")
	assert_eq(String(action["type"]), "screen", "shipped-case token stored canonical lowercase")
	assert_true(String("\n".join(PackedStringArray(wired.structured["warnings"]))).contains("auto-filled"))


func test_add_widgets_appearance_defaults_and_game_warnings() -> void:
	var first := String(_resource().get_screen_name(_resource().get_screen_ids()[0]))
	var added: McpToolResult = await _call("add_menu_widgets", { "rows": [
		{ "parent": first, "type": "BUTTON", "rect": [10, 10, 100, 25], "name": "PlainBtn" },
		{ "parent": first, "type": "CHECKBOX", "rect": [10, 40, null, 25], "name": "BareChk" },
		{ "parent": first, "type": "RADIO", "rect": [10, 70, null, 25], "name": "LabeledRad", "text": "Oops" },
	] })
	assert_false(added.is_error, str(added.content))
	var btn := int(added.structured["ids"][0])
	var rows: Array = _resource().get_widget_appearances(btn)
	assert_eq(rows.size(), 4, "Buttons default to the four shipped empty state rows.")
	assert_eq(String(rows[0]["state"]), "default")
	assert_eq(String(rows[0]["type"]), "")
	var chk := int(added.structured["ids"][1])
	assert_eq((_resource().get_widget_appearances(chk) as Array).size(), 0, "Toggles get no silent default art.")
	assert_eq(_resource().get_window_rect_flags(chk) & NovaMnuDocument.RECT_HAS_RIGHT, 0, "null width = auto-size")
	var warnings := String("\n".join(PackedStringArray(added.structured["warnings"])))
	assert_true(warnings.contains("CHECKBOX"), "Bare toggle art warning")
	assert_true(warnings.contains("sibling STATIC"), "Radio-label warning teaches the shipped pattern")


func test_game_safety_flags_broken_shapes() -> void:
	# Author the pre-fix OPENNOVA shapes straight onto the resource (the tools
	# refuse them now) and expect the analyzer to call each one out.
	var first_sid := int(_resource().get_screen_ids()[0])
	var root := int(_resource().get_screen_root_id(first_sid))
	var bad_btn := int(_resource().add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(10, 10, 100, 0)))
	_resource().set_widget_name(bad_btn, "BadJump")
	_resource().set_widget_text(bad_btn, "Crash me")
	_resource().set_widget_actions(bad_btn, [{ "type": "screen", "target": "NOWHERE", "file": "", "state": "" }])
	var bad_list := int(_resource().add_widget(root, NovaMnuDocument.TYPE_LIST, Rect2(10, 40, 200, 100)))
	_resource().set_widget_name(bad_list, "BareList")

	var analyzed: McpToolResult = await _call("analyze_menu")
	assert_false(analyzed.is_error, str(analyzed.content))
	var safety: Dictionary = analyzed.structured["game_safety"]
	assert_gt(int(safety["errors"]), 0, "The crash-class rules fire")
	var rules := {}
	for finding: Dictionary in safety["findings"]:
		rules[String(finding["rule"])] = true
	assert_true(rules.has("screen_action_file"), "empty file= on a screen action (the crash)")
	assert_true(rules.has("pressable_appearance"), "pressable without appearance rows")
	assert_true(rules.has("text_color"), "uncolored text")
	assert_true(rules.has("list_scrollbar"), "scrollbar-less list")


func test_game_safety_is_quiet_on_shipped_menus() -> void:
	# False-positive gate: a shipped menu must analyze with ZERO errors (warns
	# are tolerated only if absent here — keep shipped files fully quiet).
	var analyzed: McpToolResult = await _call("analyze_menu", { "path": _abs(JO_OPTIONS) })
	assert_false(analyzed.is_error, str(analyzed.content))
	var safety: Dictionary = analyzed.structured["game_safety"]
	assert_eq(int(safety["errors"]), 0, "No errors on jo_options.mnu: %s" % [safety["findings"]])
	assert_eq(int(safety["warnings"]), 0, "No warnings on jo_options.mnu: %s" % [safety["findings"]])
