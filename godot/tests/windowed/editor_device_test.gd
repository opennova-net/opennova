extends GutTest

## S13 V6 (the review): the editor's model device in a window, its renderer rendering and the Preview
## window's canvas drawing the device each frame (the editor's ImGui pass attached), at one unit a
## frame (a build budget of 0). The device keeps its last picture while it builds, by three rules,
## and reverting any one fails a test here:
## - a draw renders nothing while a build runs: frame after frame the SubViewport's update stays
##   disabled and the build goes a unit further (a draw that rendered would hold the build's units
##   too: a frame that renders runs none);
## - the render a frame's draw asked for before that frame's take began a build stands, the last
##   complete picture, and no unit runs in that frame;
## - a take sizes nothing while the device keeps a rendered picture: a model no canvas draws (the
##   Preview showing another one) keeps the size the canvas drew it at while it builds, never its
##   viewport's state's, and takes the state's once the build ends. The state takes a size only
##   where no canvas sizes the picture (S13 V5), so it is set once the canvas stops drawing it.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const ARMORY := "res://../fixtures/threedi/synth/armory.3di"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	# The ImGui context past its first frames before the editor attaches to it: a context whose first
	# NewFrame ran without docking asserts when docking is enabled exactly at its second (imgui.cpp's
	# frame sanity check), which an editor made in a run's first frame would meet.
	for _frame in 3:
		await get_tree().process_frame
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor device %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)
	await get_tree().process_frame


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	await get_tree().process_frame


## The viewport over `path`, its envelope.
func _state(path: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "limit": 1})


## The device drawing the model viewport over `path`: its SubViewport, null for none.
func _device(path: String) -> SubViewport:
	return _app.get_viewport_device(path, "model")


## The viewport over `path` ready on its device, a frame at a time.
func _await_ready(path: String) -> Dictionary:
	var state := _state(path)
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)):
			break
		await get_tree().process_frame
		state = _state(path)
	return state


## How far the build of the viewport over `path` has run.
func _done(path: String) -> int:
	return int(_state(path).get("device", {}).get("build", {}).get("done", -1))


## A light of the model open at `path` edited (its start colour's red), the request handled and
## nothing pumped: the editor's next frame takes the Rebuild after its canvas drew (an EditorApp frame
## draws, then pumps, then steps the builds).
func _edit_light(path: String, red: int) -> void:
	var row: int = _seam.get_row_id(0)
	var lights: PackedInt64Array = _seam.get_child_records(row, "light")
	assert_gt(lights.size(), 0, "the fixture has a light")
	if lights.is_empty():
		return
	var answer: Variant = JSON.parse_string(String(_app.request_json(JSON.stringify({"kind": "edit_record",
			"path": path, "edits": [{"op": "set", "id": lights[0], "field": "start.r", "value": red}]}))))
	assert_true(answer is Dictionary and bool((answer as Dictionary).get("ok", false)), str(answer))


## A project with `count` copies of the armory, the first open, its picture built and drawn.
func _open_armories(count: int) -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor device project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Device Game"))
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(ARMORY))
	assert_eq(DirAccess.make_dir_recursive_absolute(dir.path_join("models")), OK)
	for index in count:
		var out := FileAccess.open(dir.path_join("models/armory%d.3di" % index), FileAccess.WRITE)
		out.store_buffer(bytes)
		out.close()
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	var path := "models/armory0.3di"
	assert_true(_seam.open_document(path))
	assert_eq(String((await _await_ready(path)).get("status", "")), "ready")
	# Drawn by the Preview's canvas, built.
	for _frame in 3:
		await get_tree().process_frame
	return path


func test_a_draw_renders_nothing_while_a_build_runs() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var path := await _open_armories(1)
	var device := _device(path)
	assert_not_null(device)
	if device == null:
		return
	_app.build_budget_ms = 0
	_edit_light(path, 12)
	# The frame that takes the Rebuild (its render kept, no unit); from the next, a unit a frame, and
	# the canvas's draws render nothing.
	await get_tree().process_frame
	var state := _state(path)
	assert_eq(String(state.get("status", "")), "loading", str(state))
	var total := int(state.get("progress", {}).get("total", 0))
	assert_gt(total, 2, "a build of several units: %s" % str(state))
	var done := _done(path)
	var frames := 0
	while String(_state(path).get("status", "")) == "loading" and frames < total + 10:
		await get_tree().process_frame
		frames += 1
		if String(_state(path).get("status", "")) != "loading":
			break
		assert_eq(device.render_target_update_mode, SubViewport.UPDATE_DISABLED,
				"frame %d of the build: no render while it runs" % frames)
		var now := _done(path)
		assert_eq(now - done, 1, "a unit a frame, the canvas drawing it")
		done = now
	assert_eq(String((await _await_ready(path)).get("status", "")), "ready", "built, the canvas drawing it")


func test_a_rebuild_keeps_the_render_its_frame_asked_for() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var path := await _open_armories(1)
	var device := _device(path)
	assert_not_null(device)
	if device == null:
		return
	_app.build_budget_ms = 0
	# The canvas drew it this frame before the take began the build: that render stands (the last
	# complete picture), and no unit ran in the frame.
	_edit_light(path, 12)
	await get_tree().process_frame
	assert_eq(device.render_target_update_mode, SubViewport.UPDATE_ONCE, "the render the frame's draw asked for")
	var state := _state(path)
	assert_eq(String(state.get("status", "")), "loading", str(state))
	assert_eq(int(state.get("progress", {}).get("done", -1)), 0, "no unit in the frame that renders")
	await get_tree().process_frame
	assert_eq(_done(path), 1, "the next frame runs one")
	assert_eq(device.render_target_update_mode, SubViewport.UPDATE_DISABLED, "and renders nothing")
	assert_eq(String((await _await_ready(path)).get("status", "")), "ready")


func test_a_take_sizes_nothing_while_the_last_picture_is_kept() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var first := await _open_armories(2)
	var device := _device(first)
	assert_not_null(device)
	if device == null:
		return
	# The canvas draws it at a size of its own, so its viewport's state takes none (S13 V5).
	var drawn: Vector2i = device.size
	var sized := {"kind": "set_viewport", "path": first,
			"viewport": {"device": {"width": 123, "height": 77}}}
	assert_ne(drawn, Vector2i(123, 77), "the canvas's size is not the one the state takes")
	assert_true(bool(_state(first).get("device", {}).get("canvas_sized", false)), "sized by the canvas")
	assert_false(_seam.done(sized), "refused while a canvas sizes the picture")
	# Building at a unit a frame, then the Preview showing the second model (its build first: the most
	# recently used): the first no canvas draws while it keeps its picture.
	_app.build_budget_ms = 0
	_edit_light(first, 12)
	await get_tree().process_frame
	assert_eq(String(_state(first).get("status", "")), "loading")
	assert_true(_seam.open_document("models/armory1.3di"))
	# A whole frame undrawn, its device reports no canvas sizing it: the state takes a size of its own,
	# the one a take sizes an undrawn device to.
	for _frame in 10:
		if not bool(_state(first).get("device", {}).get("canvas_sized", true)):
			break
		await get_tree().process_frame
	assert_false(bool(_state(first).get("device", {}).get("canvas_sized", true)),
			"undrawn: no canvas sizes it")
	assert_eq(String(_state(first).get("status", "")), "loading", "still building")
	assert_true(_seam.done(sized), "no canvas sizes it: the state takes the size")
	# Each frame's take runs before its units, so every frame that began with the build running keeps
	# the size the canvas drew it at, the one that ends it included.
	var frames := 0
	while String(_state(first).get("status", "")) == "loading" and frames < 600:
		await get_tree().process_frame
		frames += 1
		assert_eq(device.size, drawn,
				"frame %d of the build undrawn: the kept picture is not sized again" % frames)
	assert_gt(frames, 0, "a take while the picture is kept")
	assert_eq(String(_state(first).get("status", "")), "ready", "built")
	# The build over, the next take sizes the undrawn device to its state's size.
	await get_tree().process_frame
	assert_eq(device.size, Vector2i(123, 77), "no picture kept: the state's size")
