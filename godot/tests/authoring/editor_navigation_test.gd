extends GutTest

## Back and Forward over the editor's shell (the navigation history, CONTEXT.md "Navigation history"):
## the mouse's back and forward buttons (MOUSE_BUTTON_XBUTTON1, XBUTTON2), pushed through the window's
## input as the OS sends them, go Back and Forward, taken at the root (EditorApp's _input) before the
## ImGui layer and any control and consumed, press and release; another button is none of theirs. The
## wire's navigate_back and navigate_forward answer the same, the navigation section saying where each
## goes. Headless: no ImGui context draws, so the menu bar's arrows and Alt+Left and Alt+Right are the
## editor_ui ctest's (tests/editor_ui/navigation_test.cpp), over the null backend.

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor navigation %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


# A new project with its required files, main.mnu opened, then a second menu made (and opened): one
# place back, the main menu.
func _two_places() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor navigation project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	if not _seam.new_project(dir.path_join("Places"), "Places"):
		return false
	_seam.create_missing_files()
	return _seam.open_document("main.mnu") and _seam.create_file("extra.mnu")


func _navigation() -> Dictionary:
	return _seam.state(["navigation"]).get("navigation", {})


func _active() -> String:
	return String(_seam.state(["documents"]).get("documents", {}).get("active", ""))


func _button(index: MouseButton, pressed: bool) -> InputEventMouseButton:
	var event := InputEventMouseButton.new()
	event.button_index = index
	event.pressed = pressed
	event.position = Vector2(400, 300)
	event.global_position = event.position
	return event


# A click of `index` pushed through the window's input, press then release: whether each was consumed.
func _click(index: MouseButton) -> Array:
	var viewport := get_viewport()
	var handled := []
	viewport.push_input(_button(index, true))
	handled.append(viewport.is_input_handled())
	viewport.push_input(_button(index, false))
	handled.append(viewport.is_input_handled())
	return handled


func test_side_buttons_go_back_and_forward() -> void:
	if _app == null:
		return
	assert_true(_two_places(), "a project with main.mnu and extra.mnu opened")
	var extra := _active()
	assert_string_contains(extra, "extra.mnu")
	var navigation := _navigation()
	assert_true(bool(navigation.get("can_back", false)), str(navigation))
	assert_false(bool(navigation.get("can_forward", true)), str(navigation))
	var back: Array = navigation.get("back", [])
	assert_eq(back.size(), 1, str(back))
	if back.is_empty():
		return
	assert_string_contains(String(back[0].get("path", "")), "main.mnu")
	assert_eq(String(back[0].get("pane", "")), "document")

	# The back button: consumed at the root, press and release, and Back.
	assert_eq(_click(MOUSE_BUTTON_XBUTTON1), [true, true], "the back button is the editor's, consumed")
	assert_string_contains(_active(), "main.mnu")
	navigation = _navigation()
	assert_false(bool(navigation.get("can_back", true)), str(navigation))
	assert_true(bool(navigation.get("can_forward", false)), str(navigation))
	var status: Dictionary = _seam.state(["status"]).get("status", {})
	assert_string_contains(str(status.get("status", "")), "Back to")

	# The forward button: Forward, to where Back left.
	assert_eq(_click(MOUSE_BUTTON_XBUTTON2), [true, true], "the forward button is the editor's, consumed")
	assert_eq(_active(), extra)
	assert_true(bool(_navigation().get("can_back", false)))

	# Nowhere forward: the press is still the editor's, and nothing moves.
	assert_eq(_click(MOUSE_BUTTON_XBUTTON2), [true, true])
	assert_eq(_active(), extra)
	assert_eq((_navigation().get("back", []) as Array).size(), 1)

	# Another button is none of theirs: a middle click moves nothing.
	_click(MOUSE_BUTTON_MIDDLE)
	assert_eq(_active(), extra)
	assert_eq((_navigation().get("back", []) as Array).size(), 1)


func test_wire_goes_where_the_buttons_go() -> void:
	if _app == null:
		return
	assert_true(_two_places(), "a project with main.mnu and extra.mnu opened")
	var extra := _active()
	assert_true(_seam.done({"kind": "navigate_back"}), "navigate_back goes back")
	assert_string_contains(_active(), "main.mnu")
	var refused: Dictionary = _seam.outcome({"kind": "navigate_back"})
	assert_false(bool(refused.get("done", true)), "nothing back: navigate_back is refused")
	assert_string_contains(str(refused.get("findings", [])), "navigation.none")
	assert_true(_seam.done({"kind": "navigate_forward", "steps": 1}), "navigate_forward goes forward")
	assert_eq(_active(), extra)
	var answer: Dictionary = _seam.request({"kind": "navigate_forward", "steps": 0})
	assert_false(bool(answer.get("ok", true)), "steps 0 is not read")
