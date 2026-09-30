extends GutTest

## The menu preview headless (ADR 0046 S9j): the editor boots with no ImGui context and
## its preview still renders through the runtime's MenuFrame, read as JSON. A new
## project's menu opens on its first screen, STARTUP, which shows its title where the
## game draws it, in its font from fonts/; the game's hit test at the title's centre
## finds it; an unsaved string table edit and an unsaved stylesheet colour show at once
## (the open documents stand in for their files); the options hold a window in a state; a
## drag of a window's handles (S9k1) writes its POSITION on the grid as one undo step;
## several selected windows move, align and change their drawing order in one undo step
## each (S9k2).

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")

var _dirs: Array[String] = []
var _app: Node = null
## The typed seam the tests knew, over the app's request_json and query_json (S13 A5).
var _seam: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor preview %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


func _preview() -> Dictionary:
	var parsed: Variant = JSON.parse_string(String(_app.get_menu_preview_json()))
	return parsed if parsed is Dictionary else {}


func _widget(preview: Dictionary, name: String) -> Dictionary:
	for widget: Variant in preview.get("widgets", []):
		if widget is Dictionary and String(widget.get("name", "")) == name:
			return widget as Dictionary
	return {}


func _string_record(table: String, key: String) -> int:
	assert_true(_seam.open_document(table))
	for i in _seam.get_row_count():
		var row: int = _seam.get_row_id(i)
		for id: int in _seam.get_child_records(row, "string"):
			if String(_seam.get_field(id, "key")) == key:
				return id
	return 0


func test_preview_follows_the_menu_its_tables_and_its_style() -> void:
	if _app == null:
		return
	assert_false(_app.is_available(), "headless: no ImGui context")
	var none := _preview()
	assert_eq(String(none.get("status", "")), "no_project", str(none))
	assert_eq(String(none.get("message", "")), "Open a project to preview its menus.")

	var dir := OS.get_cache_dir().path_join("opennova editor preview project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Preview Game"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_eq(String(_preview().get("status", "")), "no_menu")
	assert_true(_seam.open_document("main.mnu"))
	var opened := _preview()
	assert_eq(String(opened.get("status", "")), "ready", "opened, a menu shows its first screen: %s" % str(opened))
	assert_eq(String(opened.get("screen", {}).get("name", "")), "STARTUP")
	var title: int = _seam.find_record("TITLE")
	var main: int = _seam.find_record("MAIN")
	var exit: int = _seam.find_record("EXIT")
	assert_gt(title, 0)
	assert_true(_seam.select_record(title))

	# STARTUP as the game draws it.
	var preview := _preview()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_true(bool(preview.get("current", false)))
	assert_eq(String(preview.get("screen", {}).get("name", "")), "STARTUP")
	assert_eq(preview.get("missing", [1]).size(), 0, str(preview))
	assert_eq(preview.get("unreadable", [1]).size(), 0, str(preview))
	var widget := _widget(preview, "TITLE")
	assert_eq(String(widget.get("text", "")), "Preview Game", str(widget))
	assert_eq(int(widget.get("id", 0)), title)
	var rect: Array = widget.get("rect", [])
	assert_eq(rect.size(), 4, str(widget))
	if rect.size() != 4:
		return
	assert_gt(float(rect[3]), float(rect[1]), "a text-sized height")
	assert_eq(int(rect[1]), 75 + 120, "absolute: MAIN's top plus its own")
	assert_eq(String(widget.get("font", "")), "Arial16b.fnt")
	assert_eq(String(widget.get("text_color", "")), "FFFFFFFF")

	# The render check compiled the same screen headless (texture sizes from their headers):
	# every window where the preview (Godot's decoders) placed it, the same notes.
	var screen_id := int(preview.get("screen", {}).get("id", 0))
	var rendered: Variant = JSON.parse_string(String(_seam.get_menu_render_json(String(preview.get("path", "")), screen_id)))
	assert_true(rendered is Dictionary, str(rendered))
	if rendered is Dictionary:
		var render := rendered as Dictionary
		assert_eq(String(render.get("status", "")), "ready", str(render))
		assert_true(bool(render.get("current", false)), str(render))
		var ours: Array = preview.get("widgets", [])
		var theirs: Array = render.get("widgets", [])
		assert_eq(theirs.size(), ours.size())
		for i in mini(ours.size(), theirs.size()):
			var mine := ours[i] as Dictionary
			var other := theirs[i] as Dictionary
			assert_eq(String(other.get("name", "")), String(mine.get("name", "")))
			assert_eq(str(other.get("rect", [])), str(mine.get("rect", [])), String(mine.get("name", "")))
		var our_notes: Array[String] = []
		for note: Variant in preview.get("notes", []):
			our_notes.append("%s %s" % [String((note as Dictionary).get("name", "")), String((note as Dictionary).get("code", ""))])
		var their_notes: Array[String] = []
		for note: Variant in render.get("notes", []):
			their_notes.append("%s %s" % [String((note as Dictionary).get("name", "")), String((note as Dictionary).get("code", ""))])
		assert_eq(their_notes, our_notes)
		# MAIN's CUSTOM appearance: the shell's hook, a note the preview alone shows.
		assert_true(our_notes.has("MAIN appearance_custom"), str(our_notes))
	var missing_render: Variant = JSON.parse_string(String(_seam.get_menu_render_json("nope.mnu", screen_id)))
	assert_eq(String((missing_render as Dictionary).get("status", "")), "no_screen")

	# The game's hit test at its centre finds it; above MAIN there is nothing.
	var hit: Variant = JSON.parse_string(String(_app.menu_preview_hit_json(
			(float(rect[0]) + float(rect[2])) / 2.0, (float(rect[1]) + float(rect[3])) / 2.0)))
	assert_true(hit is Dictionary)
	if hit is Dictionary:
		assert_eq(int(hit.get("id", 0)), title, str(hit))
		assert_eq(String(hit.get("name", "")), "TITLE")
	var nothing: Variant = JSON.parse_string(String(_app.menu_preview_hit_json(5.0, 5.0)))
	assert_eq(int((nothing as Dictionary).get("index", 0)), -1)

	# An unsaved string table edit shows: MAIN names menutxt.bin, TITLE's STRING is an id.
	var created: Variant = JSON.parse_string(String(_app.request_json(JSON.stringify(
			{"kind": "create_file", "path": "menutxt.bin", "file_kind": "strings"}))))
	assert_true(created is Dictionary and bool((created as Dictionary).get("ok", false)), str(created))
	var exit_string := _string_record("menutxt.bin", "MM_Exit")
	assert_gt(exit_string, 0)
	assert_true(_seam.open_document("main.mnu"))
	assert_true(_seam.write_field(main, "text_rsrc"))
	assert_true(_seam.set_field(main, "text_rsrc", "menutxt.bin"))
	assert_true(_seam.set_field(title, "string.type", "ID"))
	assert_true(_seam.set_field(title, "string.value", "MM_Exit"))
	assert_eq(String(_widget(_preview(), "TITLE").get("text", "")), "Exit")
	assert_true(_seam.open_document("menutxt.bin"))
	assert_true(_seam.set_field(exit_string, "text", "Leave"))
	assert_true(_seam.is_document_dirty(), "the table edit is not saved")
	preview = _preview()
	assert_eq(String(preview.get("screen", {}).get("name", "")), "STARTUP", "the preview stays on the menu screen")
	assert_eq(String(_widget(preview, "TITLE").get("text", "")), "Leave", str(preview))

	# An unsaved stylesheet colour shows.
	assert_true(_seam.open_document("menu_style.mns"))
	var fg: int = _seam.find_record("DEF_TEXT_FG")
	assert_gt(fg, 0)
	assert_true(_seam.set_field(fg, "value", "FFFF0000"))
	assert_eq(String(_widget(_preview(), "TITLE").get("text_color", "")), "FFFF0000")

	# Options: EXIT held under the mouse, every window shown; an unknown option refused.
	assert_true(_app.set_menu_preview_options({"force_id": exit, "force_state": "mouseover", "show_hidden": true}))
	var options: Dictionary = _preview().get("options", {})
	assert_eq(int(options.get("force_id", 0)), exit, str(options))
	assert_true(bool(options.get("show_hidden", false)))
	assert_false(_app.set_menu_preview_options({"force_state": "sideways"}))
	assert_false(_app.set_menu_preview_options({"bogus": 1}))

	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_eq(String(_preview().get("status", "")), "no_project")


## S9k1: a drag of a window's handles through the preview, as the preview window writes it:
## TITLE moved by (13, 5) with the snap lands on the screen's grid (MAIN's top is 75), its
## text still sizing its height; one undo puts it back; its right edge dragged without the
## snap; an unknown handle and a record that is no window of the screen are refused; the
## held state's checked, open list and focus options.
func test_a_drag_moves_a_window_on_the_grid() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor preview drag %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Drag Game"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var title: int = _seam.find_record("TITLE")
	assert_gt(title, 0)
	assert_true(_seam.select_record(title))
	var start: Array = _widget(_preview(), "TITLE").get("rect", [])
	assert_eq(start.size(), 4, str(start))
	if start.size() != 4:
		return
	assert_eq(int(start[1]), 75 + 120)

	assert_true(_app.menu_preview_drag(title, "move", 13, 5, true))
	var moved: Array = _widget(_preview(), "TITLE").get("rect", [])
	assert_eq(moved.size(), 4, str(moved))
	if moved.size() != 4:
		return
	assert_eq(int(moved[0]) % 8, 0, "left on the grid: %s" % str(moved))
	assert_eq(int(moved[1]) % 8, 0, "top on the grid: %s" % str(moved))
	assert_eq(int(moved[0]), 16)
	assert_eq(int(moved[1]), 200)
	assert_eq(int(_seam.get_field(title, "position.left")), 16)
	assert_eq(int(_seam.get_field(title, "position.top")), 125)
	assert_null(_seam.get_field(title, "position.bottom"), "its text still sizes its height")
	assert_true(_seam.is_document_dirty())
	_seam.undo()
	assert_eq(str(_widget(_preview(), "TITLE").get("rect", [])), str(start), "one undo puts it back")
	assert_false(_seam.is_document_dirty())

	assert_true(_app.menu_preview_drag(title, "right", -100, 3, false))
	assert_eq(int(_seam.get_field(title, "position.right")), 700)
	assert_eq(int(_seam.get_field(title, "position.left")), 0)
	assert_false(_app.menu_preview_drag(title, "middle", 1, 1, true))
	assert_false(_app.menu_preview_drag(999999, "move", 1, 1, true))

	assert_true(_app.set_menu_preview_options({"force_id": title, "force_state": "selected", "checked": true,
			"popup_open": true, "focus": true}))
	var options: Dictionary = _preview().get("options", {})
	assert_eq(String(options.get("force_state", "")), "selected", str(options))
	assert_true(bool(options.get("checked", false)))
	assert_true(bool(options.get("popup_open", false)))
	assert_true(bool(options.get("focus", false)))
	assert_false(_app.set_menu_preview_options({"focus": 1}), "a flag takes true or false")

	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_eq(String(_preview().get("status", "")), "no_project")


## S9k2: several windows through the preview. TITLE and EXIT selected together: a move of
## EXIT (a selected window) takes TITLE with it, one undo step; EXIT's left edge aligned to
## TITLE's (the first window named) and EXIT sent to the back of MAIN's windows, one undo
## step each; the refusals.
func test_several_windows_move_and_arrange_in_one_step() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor preview arrange %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Arrange Game"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var main: int = _seam.find_record("MAIN")
	var title: int = _seam.find_record("TITLE")
	var exit: int = _seam.find_record("EXIT")
	assert_gt(exit, 0)
	assert_true(_seam.select_record(title))
	assert_true(_seam.select_record(exit, "add"))
	assert_eq(_seam.get_selected_records().size(), 2)
	var preview := _preview()
	var title_start: Array = _widget(preview, "TITLE").get("rect", [])
	var exit_start: Array = _widget(preview, "EXIT").get("rect", [])
	assert_eq(title_start.size(), 4, str(preview))
	assert_eq(exit_start.size(), 4, str(preview))
	if title_start.size() != 4 or exit_start.size() != 4:
		return

	# A move of EXIT moves TITLE as far.
	assert_true(_app.menu_preview_drag(exit, "move", 8, 16, false))
	preview = _preview()
	var title_moved: Array = _widget(preview, "TITLE").get("rect", [])
	var exit_moved: Array = _widget(preview, "EXIT").get("rect", [])
	assert_eq(title_moved.size(), 4, str(preview))
	assert_eq(exit_moved.size(), 4, str(preview))
	if title_moved.size() != 4 or exit_moved.size() != 4:
		return
	assert_eq(int(title_moved[0]), int(title_start[0]) + 8, str(title_moved))
	assert_eq(int(title_moved[1]), int(title_start[1]) + 16, str(title_moved))
	assert_eq(int(exit_moved[0]), int(exit_start[0]) + 8, str(exit_moved))
	assert_eq(int(exit_moved[1]), int(exit_start[1]) + 16, str(exit_moved))
	_seam.undo()
	preview = _preview()
	assert_eq(str(_widget(preview, "TITLE").get("rect", [])), str(title_start), "one undo puts both back")
	assert_eq(str(_widget(preview, "EXIT").get("rect", [])), str(exit_start))
	assert_false(_seam.is_document_dirty())

	# EXIT's left edge to TITLE's, its width kept; then EXIT first among MAIN's windows.
	assert_true(_app.menu_preview_arrange(PackedInt64Array([title, exit]), "align_left"))
	assert_eq(int(_seam.get_field(exit, "position.left")), int(_seam.get_field(title, "position.left")))
	assert_eq(int(_seam.get_field(exit, "position.right")), 120)
	assert_true(_app.menu_preview_arrange(PackedInt64Array([exit]), "send_to_back"))
	var order: PackedInt64Array = _seam.get_child_records(main, "window")
	assert_eq(order.size(), 2, str(order))
	if order.size() == 2:
		assert_eq(order[0], exit, "sent to the back: drawn first")
	_seam.undo()
	_seam.undo()
	assert_false(_seam.is_document_dirty(), "one undo step each")
	assert_eq(int(_seam.get_field(exit, "position.left")), 340)

	# Refusals: two windows cannot be distributed, an unknown op, one window cannot be
	# aligned, a record the preview does not show.
	assert_false(_app.menu_preview_arrange(PackedInt64Array([title, exit]), "distribute_horizontally"))
	assert_false(_app.menu_preview_arrange(PackedInt64Array([title, exit]), "align_middle"))
	assert_false(_app.menu_preview_arrange(PackedInt64Array([title]), "align_left"))
	assert_false(_app.menu_preview_arrange(PackedInt64Array([999999, exit]), "align_left"))
	assert_false(_seam.is_document_dirty())

	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_eq(String(_preview().get("status", "")), "no_project")
