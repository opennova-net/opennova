extends GutTest

## The OpenNova Editor's shell (ADR 0046 d4/d10) booted headless from its scene: the
## editor-enabled GDExtension variant is what a source run loads, the typed seam
## creates a project, fills its checklist, builds it, and Play starts the game on the
## build (the Godot binary at this checkout, headless and self-quitting) through the
## real process seam, whose exit the session notices.

const EDITOR_SCENE := "res://editor/editor_root.tscn"

var _dirs: Array[String] = []
var _app: Node = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	assert_true(_app.has_method("get_loaded_variant"), "the root is an EditorApp")
	var settings_dir := OS.get_cache_dir().path_join("opennova editor app %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


func test_editor_variant_boots_headless() -> void:
	if _app == null:
		return
	assert_eq(_app.get_loaded_variant(), "editor")
	assert_false(_app.is_available(), "headless: no ImGui context, the seam still works")
	assert_false(_app.is_project_open())
	assert_true(_app.is_source_run(), "a GUT run is a source run: Play drives this Godot binary")
	assert_eq(_app.get_recent_projects(), PackedStringArray())


func test_new_project_fills_builds_and_plays() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var root := dir.path_join("My Game")
	assert_true(_app.new_project(root, "My Game"))
	assert_true(_app.is_project_open())
	assert_eq(_app.get_project_title(), "My Game")
	assert_eq(_app.get_project_root(), root)
	assert_gt(_app.get_required_total(), 0)
	assert_eq(_app.get_required_missing(), _app.get_required_total(), "a new project has every required file missing")
	assert_eq(_app.get_recent_projects(), PackedStringArray([root]))

	assert_false(_app.build(), "a build is refused while required files are missing")
	assert_eq(_app.create_missing_files(), 0, "Create all missing leaves nothing missing")
	assert_true(_app.build())
	var build_dir: String = _app.get_last_build_dir()
	assert_true(FileAccess.file_exists(build_dir.path_join("localres.pff")), build_dir)
	assert_eq(_app.get_problem_count(), 0)

	# Play: the runtime is this Godot binary at the source project, headless and
	# self-quitting, so the real process seam spawns it and sees it leave.
	_app.set("play_engine_args", PackedStringArray(["--headless", "--disable-render-loop", "--quit-after", "10"]))
	assert_true(_app.play(), "\n".join(_app.get_output_lines()))
	assert_eq(_app.get_play_state(), "running")
	var waited_ms := 0
	while _app.get_play_state() != "stopped" and waited_ms < 60000:
		OS.delay_msec(100)
		waited_ms += 100
		_app.pump()
	assert_eq(_app.get_play_state(), "stopped", "\n".join(_app.get_output_lines()))
	assert_true(_app.did_game_exit_on_its_own(), "the child quit by itself (--quit-after)")
	var output := "\n".join(_app.get_output_lines())
	assert_string_contains(output, "Running: ")
	assert_string_contains(output, "The game exited.")

	_app.close_project()
	assert_false(_app.is_project_open())
	assert_true(_app.open_project(root))
	assert_eq(_app.get_required_missing(), 0)
