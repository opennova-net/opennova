extends GutTest

# The menu-authoring MCP tools, end-to-end on the real Menus workspace
# (mounted editor + canvas, headless): the headline loop builds a 2-screen
# menu with a navigating button and CLICKS through it in the Interactive
# preview; the rest covers batch-undo atomicity, validation vocabularies,
# %VAR% round-trips, items/tables, multidoc tabs, and save.

const MnuWorkspaceScript := preload("res://modtools/mnu/mnu_workspace.gd")

const ALL_WIDGETS := "res://../fixtures/mnu/all_widgets.mnu"
const JO_OPTIONS := "res://../fixtures/mnu/jo_options.mnu"
const SAVE_DIR := "user://mcp_menu_tools_test"

var ws: RefCounted
var host: Control
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
	host = add_child_autofree(Control.new())
	host.size = Vector2(800, 600)
	ws.mount_viewport(host)
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
	return ws.get("_document").get("resource")


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
	assert_eq((wired.structured["warnings"] as Array).size(), 0)

	# Read back: the tree carries the button with its action.
	var menu: McpToolResult = await _call("get_menu", { "detail": "full" })
	assert_false(menu.is_error)
	assert_eq((menu.structured["screens"] as Array).size(), 2)
	var card: McpToolResult = await _call("get_menu", { "widget": button_id })
	assert_eq(String(card.structured["actions"][0]["target"]), "SECOND")

	# Play: press the button, land on SECOND, pop back.
	var on: McpToolResult = await _call("preview_menu", { "op": "on" })
	assert_false(on.is_error, str(on.content))
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


func test_screen_ops_and_rules() -> void:
	var last_delete: McpToolResult = await _call("edit_menu_screen", { "delete": String(_resource().get_screen_name(_resource().get_screen_ids()[0])) })
	assert_true(last_delete.is_error, "The last Screen is protected.")
	await _call("edit_menu_screen", { "add": { "name": "OPTIONS" } })
	var dup: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "OPTIONS" } })
	assert_true(dup.is_error, "Duplicate Screen names are refused.")
	var shown: McpToolResult = await _call("edit_menu_screen", { "show": "OPTIONS" })
	assert_eq(String(shown.structured["visible_screen"]), "OPTIONS")
	var renamed: McpToolResult = await _call("edit_menu_screen", { "set": { "screen": "OPTIONS", "props": { "name": "SETTINGS", "music_var": 3 } } })
	assert_false(renamed.is_error, str(renamed.content))
	var sid := _resource().get_screen_ids()[1]
	assert_eq(String(_resource().get_screen_name(sid)), "SETTINGS")
	assert_eq(int(_resource().get_screen_music_var(sid)), 3)
	var deleted: McpToolResult = await _call("edit_menu_screen", { "delete": "SETTINGS" })
	assert_false(deleted.is_error)
	assert_eq(_resource().get_screen_count(), 1)


func test_interactive_preview_locks_editing() -> void:
	await _call("preview_menu", { "op": "on" })
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
	await _call("preview_menu", { "op": "off" })
	var unlocked: McpToolResult = await _call("edit_menu_screen", { "add": { "name": "X" } })
	assert_false(unlocked.is_error, "off unlocks editing")


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
