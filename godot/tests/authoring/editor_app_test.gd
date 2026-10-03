extends GutTest

## The OpenNova Editor's shell (ADR 0046 d4/d10) booted headless from its scene: the
## editor-enabled GDExtension variant is what a source run loads, the typed seam
## creates a project, fills its checklist, builds it, and Play starts the game on the
## build (the Godot binary at this checkout, headless and self-quitting) through the
## real process seam, whose exit the session notices. Build and Play are requests, as the
## windows raise them, and the session's operation (S13 A1) runs across pumps.

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
	assert_true(_app.has_method("get_loaded_variant"), "the root is an EditorApp")
	var settings_dir := OS.get_cache_dir().path_join("opennova editor app %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	if is_instance_valid(_app):
		_seam.stop_play()
		var deadline := Time.get_ticks_msec() + 10000
		while _seam.get_play_state() != "stopped" and Time.get_ticks_msec() < deadline:
			await get_tree().create_timer(0.05).timeout
			_app.pump()
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


# A request through the wire seam, as the editor MCP raises it: its answer.
func _request(request: Dictionary) -> Dictionary:
	var answer: Variant = JSON.parse_string(_app.request_json(JSON.stringify(request)))
	return answer if answer is Dictionary else {}


# `request` raised, then the session pumped until the operation it started or joined ends (a
# build steps across pumps, S13 A1), for as long as its progress moves (120 s standing still is a
# hang, as the seam's settle has it): false when the request was refused or waits on the
# unsaved-changes prompt, nothing run.
func _run_operation(request: Dictionary) -> bool:
	var outcome: Dictionary = _request(request).get("outcome", {})
	if not bool(outcome.get("done", false)):
		return false
	var id := int(outcome.get("operation", 0))
	var progress := ""
	var moved_at := Time.get_ticks_msec()
	while Time.get_ticks_msec() - moved_at < 120000:
		var text: String = _seam.get_operation_json()
		var state: Variant = JSON.parse_string(text)
		if not (state is Dictionary) or int((state as Dictionary).get("operation", {}).get("id", 0)) != id:
			break
		if text != progress:
			progress = text
			moved_at = Time.get_ticks_msec()
		_app.pump()
	return true


# Build: true when the build it ran (or joined) landed good.
func _build() -> bool:
	return _run_operation({"kind": "build"}) and _seam.is_last_build_ok()


# Play: the build first, then the game on it; true when the game started.
func _play() -> bool:
	_run_operation({"kind": "play"})
	return _seam.get_play_state() == "running"


# Play starts the game only on Windows (godot/src/authoring/child_process.h): elsewhere
# it refuses and says so, and a Play leg has no running game to drive.
func _play_refused_off_windows() -> bool:
	if OS.get_name() == "Windows":
		return false
	assert_false(_play(), "Play refuses off Windows")
	pending("Play is Windows-only (godot/src/authoring/child_process.h)")
	return true


func test_editor_variant_boots_headless() -> void:
	if _app == null:
		return
	assert_eq(_app.get_loaded_variant(), "editor")
	assert_false(_app.is_available(), "headless: no ImGui context, the seam still works")
	assert_false(_seam.is_project_open())
	assert_true(_app.is_source_run(), "a GUT run is a source run: Play drives this Godot binary")
	assert_eq(_seam.get_recent_projects(), PackedStringArray())


func test_new_project_fills_builds_and_plays() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var root := dir.path_join("My Game")
	assert_true(_seam.new_project(root, "My Game"))
	assert_true(_seam.is_project_open())
	assert_eq(_seam.get_project_title(), "My Game")
	assert_eq(_seam.get_project_root(), root)
	assert_gt(_seam.get_required_total(), 0)
	assert_eq(_seam.get_required_missing(), _seam.get_required_total(), "a new project has every required file missing")
	assert_eq(_seam.get_recent_projects(), PackedStringArray([root]))

	assert_false(_build(), "a build is refused while required files are missing")
	assert_eq(_seam.create_missing_files(), 0, "Create all missing leaves nothing missing")
	assert_true(_build())
	var build_dir: String = _seam.get_last_build_dir()
	assert_true(FileAccess.file_exists(build_dir.path_join("localres.pff")), build_dir)
	# A new project's only problems are notes: the optional files it lacks and the starter
	# style variables no blank menu names.
	var problems: Variant = JSON.parse_string(_seam.get_problems_json())
	assert_true(problems is Dictionary, str(problems))
	if problems is Dictionary:
		var answer: Dictionary = problems
		assert_eq(_seam.get_problem_count(), int(answer["total"]))
		assert_eq(int(answer["shown"]), int(answer["total"]))
		assert_eq(int(answer["counts"]["errors"]) + int(answer["counts"]["warnings"]), 0, str(answer["counts"]))
		for problem: Variant in answer["problems"]:
			var row: Dictionary = problem
			assert_eq(String(row.get("severity", "")), "info", str(row))
			assert_has(["style.unused", "requirement.optional_missing"], String(row.get("code", "")), str(row))

	# Play: the runtime is this Godot binary at the source project, headless and
	# self-quitting, so the real process seam spawns it and sees it leave.
	if _play_refused_off_windows():
		return
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--quit-after", "10"]))
	assert_true(_play(), "\n".join(_seam.get_output_lines()))
	assert_eq(_seam.get_play_state(), "running")
	var waited_ms := 0
	while _seam.get_play_state() != "stopped" and waited_ms < 60000:
		OS.delay_msec(100)
		waited_ms += 100
		_app.pump()
	assert_eq(_seam.get_play_state(), "stopped", "\n".join(_seam.get_output_lines()))
	assert_true(_seam.did_game_exit_on_its_own(), "the child quit by itself (--quit-after)")
	var output := "\n".join(_seam.get_output_lines())
	assert_string_contains(output, "Running: ")
	assert_string_contains(output, "The game exited.")

	_seam.close_project()
	assert_false(_seam.is_project_open())
	assert_true(_seam.open_project(root))
	assert_eq(_seam.get_required_missing(), 0)


## A switch that fails leaves the open project open, and new_project and open_project answer
## whether the project asked for is the one open afterwards (S13 A1: a new project's folder is
## checked, and another project read, before the open one closes).
func test_failed_switch_keeps_the_project() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor switch %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var root := dir.path_join("Kept")
	assert_true(_seam.new_project(root, "Kept"))
	assert_false(_seam.new_project(root, "Again"), "a project is there already")
	assert_true(_seam.is_project_open())
	assert_eq(_seam.get_project_root(), root)
	assert_eq(_seam.get_project_title(), "Kept")
	var empty := dir.path_join("Empty")
	assert_eq(DirAccess.make_dir_recursive_absolute(empty), OK)
	assert_false(_seam.open_project(empty), "no project there")
	assert_eq(_seam.get_project_root(), root)
	var other := dir.path_join("Other")
	assert_true(_seam.new_project(other, "Other"))
	assert_eq(_seam.get_project_root(), other)
	assert_true(_seam.open_project(root))
	assert_eq(_seam.get_project_title(), "Kept")


## A second document type through the same seam: a string table gains a section and a
## string, the text round-trips through cp1252 storage, and the file survives a reopen.
func test_strings_edit_round_trip() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova strings %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Strings acceptance"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("gametext.bin"))
	var section: int = _seam.add_record("section")
	assert_gt(section, 0)
	assert_true(_seam.set_field(section, "name", "Custom"))
	var string: int = _seam.add_record("string", section)
	assert_gt(string, 0)
	assert_true(_seam.set_field(string, "key", "HELLO"))
	assert_true(_seam.set_field(string, "text", "Hello café"))
	assert_true(_seam.set_field(string, "x", 4))
	assert_eq(_seam.get_field(string, "text"), "Hello café")
	assert_eq(_seam.get_field(string, "x"), 4)
	assert_true(_seam.is_document_dirty())
	assert_true(_seam.save_documents(), "\n".join(_seam.get_output_lines()))
	_seam.close_project()
	assert_true(_seam.open_project(dir))
	assert_true(_seam.open_document("gametext.bin"))
	var found := -1
	for i in _seam.get_row_count():
		if _seam.get_row_name(i) == "Custom":
			found = i
	assert_gt(found, -1, "the section survives a reopen")
	_seam.close_project()


## An edit leaves the validation due (S13 A3: no request runs it). Through the wire the view says
## so at once and the Problems rows stand until the pumps have run it, then move with the edit, and
## a pump after that leaves them as they are; the seam settles each request before it answers, so an
## edit through it returns validated (ADR 0046 S9e's promise, kept by the seam).
func test_edits_validate_on_the_pumps() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova validated %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Validated"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("items.def"))
	var marker: int = _seam.get_row_id(0)
	assert_gt(marker, 0)
	var before: int = _seam.get_problem_count()
	var active := String(_seam.state(["documents"]).get("documents", {}).get("active", ""))
	var answer: Variant = JSON.parse_string(_app.request_json(JSON.stringify({
		"kind": "edit_record", "path": active,
		"edits": [{"op": "set", "id": marker, "field": "type", "value": 0}],
	})))
	assert_true(answer is Dictionary and bool(answer.get("ok", false)), str(answer))
	assert_true(bool(_seam.query("operation").get("validation", {}).get("running", false)),
			"the Null marker's type cleared: the validation due")
	assert_eq(_seam.get_problem_count(), before, "the rows stand until the pumps run it")
	assert_true(_seam.settle())
	assert_eq(_seam.get_problem_count(), before + 1, _seam.get_problems_json())
	assert_string_contains(_seam.get_problems_json(), "catalog.item_type")
	_app.pump()
	assert_eq(_seam.get_problem_count(), before + 1, _seam.get_problems_json())
	assert_true(_seam.set_field(marker, "type", 4), "the marker type back")
	assert_eq(_seam.get_problem_count(), before, _seam.get_problems_json())
	assert_true(_seam.save_documents())
	_seam.close_project()


## Records at any depth through the seam (ADR 0046 S9g, S9h): a menu window inside a
## window, a reparent in and out (never into itself; a screen keeps one root window), a
## Move that changes nothing still goes through, an unset edge reads nil and is written
## again with the value it read, a window's lists are records (a second ACTION, a SOUND,
## an ITEM row), windows copy and paste, the selection joins and leaves one record at a
## time, and a drag's edits (one gesture) are one undo step.
func test_records_at_any_depth() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova depth %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Depth"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var screen: int = _seam.get_row_id(0)
	var main: int = _seam.get_child_records(screen, "window")[0]
	var title: int = _seam.find_record("TITLE")
	var exit: int = _seam.find_record("EXIT")
	assert_eq(_seam.get_record_owner(title), main)
	assert_eq(_seam.get_record_owner(main), screen)
	assert_eq(_seam.get_record_owner(screen), 0)
	assert_eq(_seam.get_child_records(main, "window"), PackedInt64Array([title, exit]))
	assert_eq(_seam.get_child_records(main, "nothing"), PackedInt64Array())
	var inner: int = _seam.add_record("window", title)
	assert_gt(inner, 0)
	assert_eq(_seam.get_record_owner(inner), title)
	assert_eq(_seam.get_selected_records(), PackedInt64Array([inner]), "a new record is selected")
	assert_true(_seam.move_record(exit, 0, inner), "EXIT into the new window")
	assert_eq(_seam.get_record_owner(exit), inner)
	assert_false(_seam.move_record(title, 0, inner), "never into itself")
	assert_true(_seam.move_record(main, 0), "the only root, where it is: nothing to move")
	assert_false(_seam.remove_record(main), "a screen keeps one root window")
	assert_true(_seam.move_record(exit, 1, main), "and back out")
	assert_eq(_seam.get_child_records(main, "window"), PackedInt64Array([title, exit]))
	assert_true(_seam.move_record(exit, 1), "a Move that changes nothing still goes through")
	var exit_left: Variant = _seam.get_field(exit, "position.left")
	assert_true(_seam.clear_field(exit, "position.left"))
	assert_eq(_seam.get_field(exit, "position.left"), null, "an unset edge reads nil")
	assert_false(_seam.clear_field(exit, "name"), "a name is always written")
	assert_true(_seam.set_field(exit, "position.left", exit_left))
	assert_eq(_seam.get_field(exit, "position.left"), null, "a Set of the value it reads leaves it out")
	assert_true(_seam.write_field(exit, "position.left"), "the written tick")
	assert_eq(_seam.get_field(exit, "position.left"), exit_left, "written again with the value it read")
	assert_eq(_seam.get_field(exit, "group"), null, "the blank menu's EXIT has no GROUP")
	assert_true(_seam.write_field(exit, "group"))
	assert_eq(_seam.get_field(exit, "group"), 0)
	assert_false(_seam.write_field(exit, "name"), "a name is always written")
	assert_true(_seam.clear_field(exit, "position.left"))
	assert_true(_seam.set_field(exit, "position.left", 12))
	assert_eq(_seam.get_field(exit, "position.left"), 12)
	# A window's lists are records: a second ACTION, a SOUND, an ITEM row (which writes
	# the ITEMS block), each with its own fields.
	var first_action: int = _seam.add_record("action", exit)
	var second_action: int = _seam.add_record("action", exit)
	assert_gt(first_action, 0)
	assert_eq(_seam.get_field(first_action, "type"), "POP_SCREEN", "a new ACTION pops the screen")
	assert_true(_seam.set_field(second_action, "type", "WINDOW"))
	assert_true(_seam.set_field(second_action, "state", "SHOW"))
	assert_true(_seam.set_field(second_action, "target", "TITLE"))
	assert_eq(_seam.get_child_records(exit, "action"), PackedInt64Array([first_action, second_action]))
	var sound: int = _seam.add_record("sound", exit)
	assert_true(_seam.set_field(sound, "file", "menu.lwf"))
	assert_eq(_seam.get_field(sound, "trigger"), "MOUSE_OVER")
	assert_eq(_seam.get_field(exit, "items"), 0, "no ITEMS yet")
	var row: int = _seam.add_record("items.item", exit)
	assert_true(_seam.set_field(row, "text", "One"))
	assert_eq(_seam.get_field(exit, "items"), 1, "the ITEMS block written with its row")
	assert_eq(_seam.get_record_name(second_action), "Action 2")
	assert_eq(_seam.get_record_owner(row), exit)
	# Windows copy and paste: TITLE pasted into EXIT is named TITLE2.
	assert_true(_seam.select_record(title))
	assert_true(_seam.copy_records())
	assert_true(_seam.paste_records(exit))
	var pasted: int = _seam.find_record("TITLE2")
	assert_gt(pasted, 0)
	assert_eq(_seam.get_record_owner(pasted), exit)
	assert_eq(_seam.get_selected_records(), PackedInt64Array([pasted]), "the pasted window is selected")
	assert_true(_seam.select_record(title))
	assert_true(_seam.select_record(exit, "add"))
	assert_eq(_seam.get_selected_records(), PackedInt64Array([title, exit]))
	assert_true(_seam.select_record(exit, "toggle"))
	assert_eq(_seam.get_selected_records(), PackedInt64Array([title]))
	assert_true(_seam.select_record(title, "toggle"))
	assert_eq(_seam.get_selected_records(), PackedInt64Array(), "the last one toggled out")
	assert_false(_seam.select_record(title, "sideways"))
	var left: Variant = _seam.get_field(title, "position.left")
	for x in [30, 40, 50]:
		var answer: Variant = JSON.parse_string(_app.request_json(JSON.stringify({
			"kind": "edit_record", "path": "main.mnu",
			"edits": [
				{"op": "set", "id": title, "field": "position.left", "value": x, "gesture": 7001},
				{"op": "set", "id": title, "field": "position.right", "value": x + 300, "gesture": 7001},
			],
		})))
		assert_true(answer is Dictionary and bool(answer.get("ok", false)), str(answer))
	_seam.end_edit()
	assert_eq(_seam.get_field(title, "position.left"), 50)
	_seam.undo()
	assert_eq(_seam.get_field(title, "position.left"), left, "the drag was one undo step")
	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard


## A new document's name is checked before anything is written (a plain name inside the
## project, up to the archive's 16 bytes, an extension that names the kind), the refusal
## comes back as the request's outcome, and a new menu of its own gets one screen named
## after the file, with no copy of STARTUP and no Exit button. A font (S11b) is made and
## not opened, and create_file says it was made.
func test_create_file_names_and_a_menu_of_its_own() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova create names %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Names"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_false(_seam.create_file("../x.mnu"))
	assert_false(_seam.create_file("abcdefghijklm.mnu"), "17 bytes: past the archive's 16")
	var answer: Variant = JSON.parse_string(_app.request_json(JSON.stringify(
			{"kind": "create_file", "path": "foo.mnu", "file_kind": "strings"})))
	assert_true(answer is Dictionary and bool(answer.get("ok", false)), str(answer))
	if answer is Dictionary:
		var outcome: Dictionary = answer.get("outcome", {})
		assert_false(bool(outcome.get("done", true)), str(answer))
		var findings: Array = outcome.get("findings", [])
		assert_eq(findings.size(), 1, str(answer))
		if findings.size() == 1:
			assert_eq(String(findings[0].get("code", "")), "document.kind")
	assert_false(FileAccess.file_exists(dir.path_join("x.mnu")))
	assert_false(FileAccess.file_exists(dir.path_join("menus/abcdefghijklm.mnu")))
	assert_false(FileAccess.file_exists(dir.path_join("menus/foo.mnu")))
	assert_false(FileAccess.file_exists(dir.path_join("strings/foo.mnu")))
	assert_true(_seam.create_file("extra.mnu"), "\n".join(_seam.get_output_lines()))
	assert_true(FileAccess.file_exists(dir.path_join("menus/extra.mnu")))
	assert_eq(_seam.get_row_count(), 1)
	assert_eq(_seam.get_row_name(0), "EXTRA")
	assert_gt(_seam.find_record("MAIN"), 0)
	assert_eq(_seam.find_record("EXIT"), 0, "no Exit button")
	assert_eq(_seam.find_record("STARTUP"), 0, "no copy of the startup screen")
	# A kind the editor makes but does not edit (a font): made, not opened; the menu stays active.
	assert_true(_seam.create_file("Extra.fnt"), "\n".join(_seam.get_output_lines()))
	assert_true(FileAccess.file_exists(dir.path_join("fonts/Extra.fnt")))
	assert_eq(_seam.get_row_name(0), "EXTRA", "the menu is still the active document")
	assert_true(_seam.create_file("Extra.fnt"), "a file that is there already is no failure")
	_seam.close_project()

## The John Smith milestone (ADR 0046 S6): a new project, every required file created,
## the startup menu's title edited and a button added, a string added, the project
## built and played, the running game showing the edited menu, Exit ending the run. S9h2:
## the button gains a second ACTION and a SOUND and a new list an ITEM, read back from the
## saved file before the build, the running list showing its item.
func test_john_smith_menu_reaches_the_play_child() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova john smith %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "John Smith"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.open_document("main.mnu"))
	var screen: int = _seam.get_row_id(0)
	assert_eq(_seam.get_record_name(screen), "STARTUP")
	var title: int = _seam.find_record("TITLE")
	assert_gt(title, 0)
	assert_eq(_seam.get_field(title, "string.value"), "John Smith")
	assert_true(_seam.set_field(title, "string.value", "John Smith's Game"))
	# The screen holds its root windows; a new window goes inside a window.
	var main: int = _seam.get_child_records(screen)[0]
	assert_eq(_seam.get_record_name(main), "MAIN")
	var later: int = _seam.add_record("window", main)
	assert_gt(later, 0)
	assert_eq(_seam.get_record_owner(later), main)
	assert_true(_seam.set_field(later, "name", "LATER"))
	assert_true(_seam.set_field(later, "type", "button"))
	assert_true(_seam.set_field(later, "string.value", "Later"))
	assert_true(_seam.set_field(later, "position.left", 340))
	assert_true(_seam.set_field(later, "position.top", 430))
	assert_true(_seam.set_field(later, "position.right", 460))
	assert_eq(_seam.get_child_records(main, "window").size(), 3)
	# The menu editor's lists (S9h2), authored end to end: LATER gains a second ACTION (a
	# new one goes back; this one shows TITLE) and a SOUND, and a new LIST window an ITEM;
	# saved, read back from the file, built and played, the screen still boots.
	var back_action: int = _seam.add_record("action", later)
	var show_action: int = _seam.add_record("action", later)
	assert_eq(_seam.get_field(back_action, "type"), "POP_SCREEN")
	assert_true(_seam.set_field(show_action, "type", "WINDOW"))
	assert_true(_seam.set_field(show_action, "state", "SHOW"))
	assert_true(_seam.set_field(show_action, "target", "TITLE"))
	var sound: int = _seam.add_record("sound", later)
	assert_true(_seam.set_field(sound, "file", "menu.lwf"))
	var choices: int = _seam.add_record("window", main)
	assert_true(_seam.set_field(choices, "name", "CHOICES"))
	assert_true(_seam.set_field(choices, "type", "list"))
	for edge in [["position.left", 340], ["position.top", 470], ["position.right", 460], ["position.bottom", 520]]:
		assert_true(_seam.set_field(choices, edge[0], edge[1]))
	var item: int = _seam.add_record("items.item", choices)
	assert_true(_seam.set_field(item, "text", "One"))
	assert_eq(_seam.get_field(choices, "items"), 1, "the ITEMS block written with its row")
	assert_true(_seam.open_document("gametext.bin"))
	var section: int = _seam.add_record("section")
	assert_true(_seam.set_field(section, "name", "Menu"))
	var welcome: int = _seam.add_record("string", section)
	assert_true(_seam.set_field(welcome, "key", "JS_WELCOME"))
	assert_true(_seam.set_field(welcome, "text", "Welcome, John"))
	assert_true(_seam.save_documents(), "\n".join(_seam.get_output_lines()))
	# Read back from the saved file: the lists are there, in order, with their values.
	var reloaded: Variant = JSON.parse_string(_app.request_json(JSON.stringify({"kind": "reload_document", "path": "main.mnu"})))
	assert_true(reloaded is Dictionary and bool(reloaded.get("ok", false)), str(reloaded))
	later = _seam.find_record("LATER")
	choices = _seam.find_record("CHOICES")
	assert_gt(later, 0)
	assert_gt(choices, 0)
	var actions: PackedInt64Array = _seam.get_child_records(later, "action")
	assert_eq(actions.size(), 2)
	if actions.size() == 2:
		assert_eq(_seam.get_field(actions[0], "type"), "POP_SCREEN")
		assert_eq(_seam.get_field(actions[1], "type"), "WINDOW")
		assert_eq(_seam.get_field(actions[1], "state"), "SHOW")
		assert_eq(_seam.get_field(actions[1], "target"), "TITLE")
	var sounds: PackedInt64Array = _seam.get_child_records(later, "sound")
	assert_eq(sounds.size(), 1)
	if sounds.size() == 1:
		assert_eq(_seam.get_field(sounds[0], "file"), "menu.lwf")
	var items: PackedInt64Array = _seam.get_child_records(choices, "items.item")
	assert_eq(items.size(), 1)
	if items.size() == 1:
		assert_eq(_seam.get_field(items[0], "text"), "One")
	assert_true(_build(), "\n".join(_seam.get_output_lines()))
	if _play_refused_off_windows():
		return
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--max-fps", "60"]))
	assert_true(_play(), "\n".join(_seam.get_output_lines()))
	var client: RefCounted = null
	var deadline := Time.get_ticks_msec() + 30000
	while Time.get_ticks_msec() < deadline:
		client = preload("res://tests/mcp/mcp_test_client.gd").new()
		if await client.connect_to(get_tree(), _seam.get_play_mcp_port()):
			break
		client.close()
		client = null
		await get_tree().create_timer(0.1).timeout
	assert_not_null(client, "\n".join(_seam.get_output_lines()))
	if client != null:
		assert_not_null(await client.initialize(get_tree()))
		# The startup screen shows the edited title and the new button.
		var state: Variant = null
		deadline = Time.get_ticks_msec() + 30000
		while Time.get_ticks_msec() < deadline:
			var envelope: Variant = await client.call_tool(get_tree(), "game_menu", {"op": "state"})
			if envelope is Dictionary:
				var content: Dictionary = envelope.get("result", {}).get("structuredContent", {})
				if content.get("visible", false) and content.get("widgets", []).size() > 0:
					state = content
					break
			await get_tree().create_timer(0.2).timeout
		assert_not_null(state, "the menu never became visible")
		if state is Dictionary:
			assert_eq(str(state.get("screen", "")), "STARTUP")
			var texts := {}
			var item_counts := {}
			for widget in state.get("widgets", []):
				texts[str(widget.get("name", ""))] = str(widget.get("text", ""))
				item_counts[str(widget.get("name", ""))] = int(widget.get("items", 0))
			assert_eq(texts.get("TITLE", ""), "John Smith's Game")
			assert_eq(texts.get("LATER", ""), "Later")
			assert_eq(item_counts.get("CHOICES", 0), 1, "the new list shows its one item")
			# Exit ends the game on its own.
			var pressed: Variant = await client.call_tool(get_tree(), "game_menu", {"op": "press", "name": "EXIT"})
			assert_not_null(pressed)
		client.close()
	deadline = Time.get_ticks_msec() + 15000
	while _seam.get_play_state() != "stopped" and Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.05).timeout
		_app.pump()
	assert_eq(_seam.get_play_state(), "stopped")
	assert_true(_seam.did_game_exit_on_its_own(), "Exit quits the game")
	_seam.close_project()

func test_catalog_edits_reach_the_play_child() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova catalog play %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Catalog acceptance"))
	assert_eq(_seam.create_missing_files(), 0)
	assert_true(_seam.create_file("ammo.def"))
	var ammo_id: int = _seam.add_record("ammo")
	assert_gt(ammo_id, 0)
	assert_true(_seam.set_field(ammo_id, "name", "AMMO_CATALOG_TEST"))
	assert_true(_seam.set_field(ammo_id, "velocity", 1250))
	assert_true(_seam.open_document("weapon.def"))
	var weapon_id: int = _seam.add_record("weapon")
	assert_true(_seam.set_field(weapon_id, "weapon_name", "WPN_CATALOG_TEST"))
	assert_true(_seam.set_field(weapon_id, "round_type", "AMMO_CATALOG_TEST"))
	assert_true(_seam.set_field(weapon_id, "clipsize", 37))
	assert_true(_seam.set_field(weapon_id, "weaponweight", 2.5))
	assert_eq(_seam.get_field(weapon_id, "weaponweight_fp16"), 163840)
	var action_id: int = _seam.add_record("action", weapon_id)
	assert_true(_seam.set_field(action_id, "name", "Fire"))
	assert_true(_seam.set_field(action_id, "function", "Shoot"))
	assert_true(_seam.set_field(action_id, "ctrl_increment", 2))
	_seam.undo()
	assert_eq(_seam.get_field(action_id, "ctrl_increment"), 0)
	_seam.redo()
	assert_eq(_seam.get_field(action_id, "ctrl_increment"), 2)
	assert_true(_seam.open_document("items.def"))
	var row: int = _seam.add_record("item")
	var item_id: int = _seam.get_field(row, "id")
	assert_true(_seam.set_field(row, "display_name", "Catalog marker"))
	assert_true(_seam.is_document_dirty())
	assert_false(_build(), "unsaved catalog edits make Build wait on the unsaved prompt")
	assert_true(_seam.has_unsaved_prompt())
	_seam.resolve_unsaved(0) # Save: the edited catalogs, then the build
	assert_false(_seam.has_unsaved_prompt())
	assert_false(_seam.is_document_dirty())
	assert_true(_build(), "\n".join(_seam.get_output_lines()))
	var built_root: String = _seam.get_last_build_dir()
	if _play_refused_off_windows():
		return
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--max-fps", "60"]))
	assert_true(_play(), "\n".join(_seam.get_output_lines()))
	var client: RefCounted = null
	var deadline := Time.get_ticks_msec() + 30000
	while Time.get_ticks_msec() < deadline:
		client = preload("res://tests/mcp/mcp_test_client.gd").new()
		if await client.connect_to(get_tree(), _seam.get_play_mcp_port()):
			break
		client.close()
		client = null
		await get_tree().create_timer(0.1).timeout
	assert_not_null(client, "\n".join(_seam.get_output_lines()))
	if client != null:
		var initialized: Variant = await client.initialize(get_tree())
		assert_not_null(initialized)
		var envelope: Variant = await client.call_tool(get_tree(), "game_probe", {
			"op": "run", "name": "catalog_readback", "args": {
				"item_id": item_id, "item_name": "Catalog marker",
				"weapon_name": "WPN_CATALOG_TEST", "weapon_clipsize": 37,
				"ammo_name": "AMMO_CATALOG_TEST", "ammo_velocity": 1250,
			},
		})
		assert_not_null(envelope)
		var started: Dictionary = envelope.get("result", {}).get("structuredContent", {}) if envelope is Dictionary else {}
		assert_true(started.has("run_id"), str(envelope))
		if started.has("run_id"):
			var verdict: Variant = null
			deadline = Time.get_ticks_msec() + 30000
			while verdict == null and Time.get_ticks_msec() < deadline:
				envelope = await client.call_tool(get_tree(), "game_probe", {
					"op": "status", "run_id": started["run_id"], "wait_ms": 500,
				})
				if envelope is Dictionary:
					verdict = envelope.get("result", {}).get("structuredContent", {}).get("verdict")
			assert_not_null(verdict, str(envelope))
			if verdict is Dictionary:
				assert_true(bool(verdict.get("ok", false)), str(verdict))
				assert_eq(str(verdict.get("data", {}).get("root", "")).replace("\\", "/").trim_suffix("/"), built_root.trim_suffix("/"))
		client.close()
	_seam.stop_play()
	deadline = Time.get_ticks_msec() + 10000
	while _seam.get_play_state() != "stopped" and Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.05).timeout
		_app.pump()
	assert_eq(_seam.get_play_state(), "stopped")
	_seam.close_project()
	assert_false(_seam.is_project_open())
	assert_true(_seam.open_project(dir))
	assert_true(_seam.open_document("items.def"))
	assert_eq(_seam.get_row_name(1), "Catalog marker")
	var reloaded_row: int = _seam.get_row_id(1)
	assert_true(_seam.set_field(reloaded_row, "hp", 50))
	_seam.close_project()
	assert_true(_seam.has_unsaved_prompt())
	_seam.resolve_unsaved(2) # Cancel
	assert_true(_seam.is_project_open())
	_seam.close_project()
	_seam.resolve_unsaved(1) # Discard
	assert_false(_seam.is_project_open())
