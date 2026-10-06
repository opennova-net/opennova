extends GutTest

## DI-14 (ADR 0046): the effect viewport's device in a window, the game's particle renderer drawing in an
## editor SubViewport (the plan's first task: its compositor passes run there). A particle file opened, its
## effect played by the editor core's effect scene: the Preview window's canvas draws the device each
## frame, the renderer's camera-side compositor pass runs over the device's camera and draws the live
## particles, and the SubViewport's picture holds them (pixels brighter than the clear colour about the
## spawn point). The device draws single-sampled, as the game's view does: a multisampled SubViewport's
## depth is no attachment the passes can bind, and every particle went undrawn.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const PTL := "particles/glow.ptl"
## The waits are on the preview clock and the scene, never a frame count: the clock runs on the frames'
## own time, and a window behind others draws without the vsync cap, so a fixed count of frames can pass
## in a fraction of a second (270 frames held exactly 8 particles at the glow's 30 a second). One second
## of preview time (62.5 ticks a second) holds about 30. The wall-clock bound only stops a stuck clock
## from hanging the run.
const PLAY_TICKS := 63
const LIVE_PARTICLES := 8
const WAIT_MS := 30000

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	for _frame in 3:
		await get_tree().process_frame
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor effect device %d" % Time.get_ticks_usec())
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


func _write(path: String, bytes: PackedByteArray) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "wrote %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


## A slow, large, additive glow that emits for a long while: big quads about the spawn point.
func _ptl() -> String:
	var text := "[effectdef]\n{\n\tid = Glow;\n\tpdefs = GlowDot;\n}\n\n"
	text += "[particledef]\n{\n\tid = GlowDot;\n\tflags = FOREVEREMIT;\n\temit_dur = 30;\n\temit_rate = 30;\n"
	text += "\temit_burst = 1;\n\tage = 2.0;\n\tscale = 1.5;\n\tspeed = 0.3;\n\tspread = 180;\n"
	text += "\tcolor1 = 255, 255, 255;\n\tcolor2 = 255, 255, 255;\n\tcolor3 = 255, 255, 255;\n\tcolor4 = 255, 255, 255;\n"
	text += "\tgraphic1 = particle_dot.tga, additive;\n\tg1_alpha = 1;\n\tg1_scale = 1.5;\n}\n\n"
	return text


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": PTL, "kind": "effect", "limit": 1})


## A second of the effect played on the preview clock, its particles alive, its device attached.
func _played() -> bool:
	var state := _state()
	var play: Dictionary = state.get("body", {}).get("play", {})
	return int(play.get("age", 0)) >= PLAY_TICKS and int(play.get("particles", 0)) > LIVE_PARTICLES \
			and bool(state.get("device", {}).get("attached", false))


## Frames pass until `condition` holds, or the wall-clock bound fails the test.
func _wait_until(condition: Callable, what: String) -> bool:
	var deadline := Time.get_ticks_msec() + WAIT_MS
	while not condition.call():
		if Time.get_ticks_msec() > deadline:
			fail_test("timed out waiting for %s" % what)
			return false
		await get_tree().process_frame
	return true


func test_the_particle_renderer_draws_in_the_effect_device() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor effect device project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Effect Device"))
	_write(dir.path_join(PTL), _ptl().to_utf8_buffer())
	_write(dir.path_join("particles/particle_dot.tga"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/cbin/particle_dot.tga")))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.open_document(PTL))
	assert_true(_seam.done({"kind": "set_viewport", "path": PTL,
			"viewport": {"kind": "effect", "options": {"grid": false}, "camera": {"distance": 6.0, "target": [0, 0.5, 0]}}}))
	if not await _wait_until(_played, "a second of the effect on the preview clock"):
		return
	var state := _state()
	assert_eq(String(state.get("status", "")), "ready")
	assert_gt(int(state.get("body", {}).get("play", {}).get("particles", 0)), LIVE_PARTICLES,
			"the scene holds live particles")
	var device: SubViewport = _app.get_viewport_device(PTL, "effect")
	assert_not_null(device, "the device drawn")
	if device == null:
		return
	var renderers := device.find_children("*", "ParticleRenderer", true, false)
	assert_eq(renderers.size(), 1)
	if renderers.size() != 1:
		return
	var renderer := renderers[0] as ParticleRenderer
	# The device hands the scene to the renderer on its next drawn frame: wait for the compiled quads.
	await _wait_until(func() -> bool: return renderer.get_rendered_quad_count() > 0, "the renderer's quads")
	var report: Dictionary = renderer.get_debug_draw_list_report()
	gut.p("draw list report: %s" % JSON.stringify(report))
	assert_gt(renderer.get_rendered_quad_count(), 0, "the renderer compiled the particles")
	RenderingServer.force_draw(false)
	var image := device.get_texture().get_image()
	assert_not_null(image)
	if image == null:
		return
	var lit := 0
	for y in range(0, image.get_height(), 4):
		for x in range(0, image.get_width(), 4):
			var c := image.get_pixel(x, y)
			if c.r + c.g + c.b > 1.2:
				lit += 1
	gut.p("lit samples: %d of %d" % [lit, (image.get_height() / 4) * (image.get_width() / 4)])
	assert_gt(lit, 10, "the picture holds the drawn particles")
