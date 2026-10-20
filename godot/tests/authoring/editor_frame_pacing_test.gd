extends GutTest

## The editor's frame pacing (EditorApp's frame_pacing, the speed pass): Engine's max_fps the display's rate
## while the editor is active, 20 once it has been quiet for 30 frames, the value it found put back as the
## node leaves the tree. The scene turns it on and a headless run drops it, so neither the game nor a test
## paces unless it turns it on; a bare EditorApp never touches max_fps. A quiet editor goes idle and a wire
## call wakes it; the preview clock playing over a picture keeps it active (headless, every preview target
## holds a device, and one holding its picture stands for one drawn), paused it lets the editor idle.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const ARMORY := "res://../fixtures/threedi/synth/armory.3di"
## EditorApp's kIdleFps and kQuietFrames.
const IDLE_FPS := 20
const QUIET_FRAMES := 30

var _dirs: Array[String] = []
var _max_fps := 0


func before_each() -> void:
	_max_fps = Engine.max_fps


func after_each() -> void:
	Engine.max_fps = _max_fps
	await EditorFixture.teardown(self, _dirs)


func _frames(count: int) -> void:
	for _frame in count:
		await get_tree().process_frame


func test_the_scene_paces_a_windowed_editor_alone() -> void:
	var packed := load(EditorFixture.EDITOR_SCENE) as PackedScene
	assert_not_null(packed)
	if packed == null:
		return
	var app := packed.instantiate()
	assert_true(bool(app.get("frame_pacing")), "the scene turns it on")
	var settings_dir := OS.get_cache_dir().path_join("opennova editor pacing %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	Engine.max_fps = 123
	add_child_autofree(app)
	await _frames(QUIET_FRAMES + 3)
	assert_false(bool(app.get("frame_pacing")), "headless: dropped as it became ready")
	assert_eq(app.get_frame_pacing_state(), "off")
	assert_eq(Engine.max_fps, 123, "max_fps untouched")


func test_a_bare_editor_app_never_touches_max_fps() -> void:
	Engine.max_fps = 123
	var app: Node = ClassDB.instantiate("EditorApp")
	assert_not_null(app)
	if app == null:
		return
	assert_false(bool(app.get("frame_pacing")), "off by default")
	var settings_dir := OS.get_cache_dir().path_join("opennova editor bare pacing %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child(app)
	await _frames(QUIET_FRAMES + 3)
	assert_eq(Engine.max_fps, 123, "a bare EditorApp paces nothing")
	app.free()
	assert_eq(Engine.max_fps, 123, "nor puts anything back")


func test_a_quiet_editor_goes_idle_and_a_wire_call_wakes_it() -> void:
	var seam: RefCounted = await EditorFixture.boot(self, "pacing quiet", _dirs)
	if seam == null:
		return
	var app: Node = seam.app
	Engine.max_fps = 0
	app.set("frame_pacing", true)
	await _frames(2)
	assert_eq(app.get_frame_pacing_state(), "active", "active as it starts")
	assert_gt(Engine.max_fps, IDLE_FPS, "the display's rate (headless: the fallback)")
	await _frames(QUIET_FRAMES + 2)
	assert_eq(app.get_frame_pacing_state(), "idle", "no project, nothing asked: quiet")
	assert_eq(Engine.max_fps, IDLE_FPS)
	app.query_json("perf")
	await _frames(1)
	assert_eq(app.get_frame_pacing_state(), "active", "the MCP's query wakes it")
	assert_gt(Engine.max_fps, IDLE_FPS)
	# Out of the tree (autofree frees it after), the node puts back the max_fps it found.
	app.get_parent().remove_child(app)
	assert_eq(Engine.max_fps, 0, "max_fps put back as the node leaves the tree")


func test_a_playing_clock_over_a_picture_keeps_it_active() -> void:
	var seam: RefCounted = await EditorFixture.boot(self, "pacing clock", _dirs)
	if seam == null:
		return
	var app: Node = seam.app
	var root := EditorFixture.new_project(self, seam, _dirs, "Pacing")
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(ARMORY))
	assert_eq(DirAccess.make_dir_recursive_absolute(root.path_join("models")), OK)
	var file := FileAccess.open(root.path_join("models/armory.3di"), FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(seam.settle(), "the model scanned")
	assert_true(seam.open_document("models/armory.3di"))
	var state: Dictionary = await seam.await_viewport({"path": "models/armory.3di", "limit": 200})
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_true(seam.settle(), "validated")
	Engine.max_fps = 0
	app.set("frame_pacing", true)
	await _frames(QUIET_FRAMES + 5)
	assert_eq(app.get_frame_pacing_state(), "active", "the clock plays over the model's picture")
	assert_true(seam.done({"kind": "set_viewport", "viewport": {"clock": {"playing": false}}}), "the clock paused")
	await _frames(QUIET_FRAMES + 5)
	assert_eq(app.get_frame_pacing_state(), "idle", "paused, the picture keeps nothing awake")
	assert_eq(Engine.max_fps, IDLE_FPS)
	app.set("frame_pacing", false)
	assert_eq(Engine.max_fps, 0, "turned off, max_fps put back")
