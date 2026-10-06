extends GutTest

## The effect viewport's device headless through the editor's wire seam (ADR 0046 DI-14): a particle file
## opened is played by the editor core's effect scene (the engine's own, spawned at tick 0 and stepped on
## the preview clock), and its device draws that scene through the game's particle renderer
## (authoring/preview_effects) in an offscreen SubViewport, the graphic read from the project's files; the
## renderer compiles the live particles into its draw list; the viewport's camera and grid reach the
## device at the next pump; another effect of the file shown is a scene anew on the same device.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const PTL := "particles/puff.ptl"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor effect %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


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


## A particle file of two effects over one definition each: Puff, a burst of dots of the fixture's
## graphic that emits for a second, and Glow, which emits for longer.
func _ptl() -> String:
	var text := ""
	for row in [["Puff", "PuffDot", 1.0], ["Glow", "GlowDot", 3.0]]:
		text += "[effectdef]\n{\n\tid = %s;\n\tpdefs = %s;\n}\n\n" % [row[0], row[1]]
		text += "[particledef]\n{\n\tid = %s;\n\temit_dur = %s;\n\temit_rate = 40;\n\temit_burst = 1;\n" % [row[1], row[2]]
		text += "\tage = 1.0;\n\tscale = 1.0;\n\tspeed = 1.5;\n\tspread = 40;\n\tgraphic1 = particle_dot.tga, blend;\n"
		text += "\tg1_alpha = 1;\n\tg1_scale = 1;\n}\n\n"
	return text


func _new_project() -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor effect project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Effect Viewport"))
	_write(dir.path_join(PTL), _ptl().to_utf8_buffer())
	var dot := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/cbin/particle_dot.tga"))
	assert_gt(dot.size(), 18, "the fixture's particle graphic reads")
	_write(dir.path_join("particles/particle_dot.tga"), dot)
	_seam.request({"kind": "rescan"})
	return dir


func _viewport() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": PTL, "kind": "effect"})


## The viewport ready on its device: a frame at a time until its device took the scene.
func _await_ready() -> Dictionary:
	var state := _viewport()
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport()
	return state


func _renderer() -> ParticleRenderer:
	var device: SubViewport = _app.get_viewport_device(PTL, "effect")
	if device == null:
		return null
	var found := device.find_children("*", "ParticleRenderer", true, false)
	return found[0] as ParticleRenderer if not found.is_empty() else null


func _grid() -> MeshInstance3D:
	var device: SubViewport = _app.get_viewport_device(PTL, "effect")
	if device == null:
		return null
	var found := device.find_children("Grid", "MeshInstance3D", true, false)
	return found[0] as MeshInstance3D if not found.is_empty() else null


func test_an_open_particle_file_plays_through_the_particle_renderer() -> void:
	if _app == null:
		return
	_new_project()
	assert_true(_seam.open_document(PTL), "the particle file opens")
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", "its effect viewport is ready")
	var body: Dictionary = state.get("body", {})
	assert_eq(String(body.get("effect", "")), "Puff", "the file's first effect")
	assert_true(bool(body.get("resolved", {}).get("this_file", false)), "the game spawns this file's Puff")
	# The clock runs a few frames: the scene plays, and the renderer compiles its particles.
	var renderer := _renderer()
	assert_not_null(renderer, "its device draws through the game's particle renderer")
	if renderer == null:
		return
	for _frame in 20:
		_app.pump()
		await get_tree().process_frame
	state = _viewport()
	body = state.get("body", {})
	assert_gt(int(body.get("play", {}).get("particles", 0)), 0, "the effect scene holds live particles")
	assert_gt(int(body.get("play", {}).get("tick", 0)), 0, "played on the preview clock's ticks")
	assert_gt(renderer.get_rendered_quad_count(), 0, "the renderer drew them")
	assert_gt(renderer.get_draw_command_count(), 0, "in material runs")
	assert_eq((body.get("missing_graphics", []) as Array).size(), 0, "the graphic read from the project's files")
	# The grid and the camera reach the device at the next pump.
	var grid := _grid()
	assert_not_null(grid)
	assert_true(grid.visible, "the grid shows by default")
	assert_true(_seam.done({"kind": "set_viewport", "path": PTL,
			"viewport": {"kind": "effect", "options": {"grid": false}, "camera": {"distance": 3.0, "target": [0, 0, 0]}}}))
	_app.pump()
	assert_false(grid.visible, "the grid hidden")
	var camera := (_app.get_viewport_device(PTL, "effect") as SubViewport).get_camera_3d()
	assert_not_null(camera)
	assert_almost_eq(camera.global_position.length(), 3.0, 0.01, "the camera where the viewport's orbit puts it")
	# Another effect of the file: the scene opened anew, drawn by the same renderer.
	assert_true(_seam.done({"kind": "set_viewport", "path": PTL, "viewport": {"kind": "effect", "options": {"effect": "Glow"}}}))
	for _frame in 10:
		_app.pump()
		await get_tree().process_frame
	state = _viewport()
	assert_eq(String(state.get("body", {}).get("effect", "")), "Glow")
	assert_eq(_renderer(), renderer, "the same device")
	assert_gt(renderer.get_rendered_quad_count(), 0, "Glow drawn")
