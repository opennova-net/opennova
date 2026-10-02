extends GutTest

## S14 V8 (ADR 0046 S14, E13): the editor's mission device in a window, its renderer rendering and the
## Document window's canvas drawing the device each frame (the editor's ImGui pass attached), over a
## project minted from the fixtures (the mission, its terrain and textures, its env, the item table,
## the models). A tests/windowed/ script, run by `scripts/test_godot.sh --suite core --windowed`.
## - The picture shows the terrain: where the terrain lies, the picture with the terrain layer shown
##   differs from the picture with it off (the option an Update, nothing built again).
## - The render token: with a model drawn in the Preview window in the same frames, the two devices
##   are of two scene states (the model's the shipped defaults, the mission's its own), so each frame
##   one of them renders and the other keeps its last picture, in turn; in the model's frames the
##   globals hold the shipped defaults again (project.godot's values), no environment their writer (a
##   mission's would otherwise stay: the globals are process-wide and written once per environment
##   change), in the mission's frames the mission's environment is the writer; the water's mirror
##   pass is on only in the frames the mission presents.
## - A mission building beside a model never lights it (review M1): every frame of its build the
##   globals hold the model's shipped values.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION := "res://../fixtures/bms/synth_logic.bms"
const FIXTURES := "res://../fixtures/"
const TERRAIN_TEXTURES := ["mnml_c", "mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1", "mnml_t"]
const MISSION_PATH := "missions/synth_logic.bms"
const MODEL_PATH := "models/pump.3di"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	# The ImGui context past its first frames before the editor attaches to it (see
	# editor_device_test.gd).
	for _frame in 3:
		await get_tree().process_frame
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor mission device %d" % Time.get_ticks_usec())
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


func _state(path: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "limit": 1})


func _await_ready(path: String) -> Dictionary:
	var state := _state(path)
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)):
			break
		await get_tree().process_frame
		state = _state(path)
	return state


func _write(path: String, bytes: PackedByteArray) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "wrote %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


func _copy_fixture(source: String, target: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FIXTURES + source))
	assert_false(bytes.is_empty(), "the fixture is available: " + source)
	_write(target, bytes)


## A texture `size` a side, grey, as an uncompressed true-colour TGA (the runtime fixture's minting).
func _tga(size: int) -> PackedByteArray:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.5, 0.5, 0.5, 1.0))
	var header := PackedByteArray()
	header.resize(18)
	header[2] = 2
	header.encode_u16(12, size)
	header.encode_u16(14, size)
	header[16] = 32
	header[17] = 0x28
	return header + image.get_data()


## A project holding the mission and what its picture reads, the mission open and ready, drawn by the
## Document window's canvas.
func _open_mission() -> SubViewport:
	var dir := OS.get_cache_dir().path_join("opennova editor mission device project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Mission Device Game"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_copy_fixture("bms/synth_logic.bms", root.path_join(MISSION_PATH))
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	var small := _tga(32)
	var large := _tga(2048)
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"), large if name == "mnml_c" or name == "mnml_d1" else small)
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	_copy_fixture("def/items.def", root.path_join("defs").path_join("items.def"))
	for name in ["pump.3di", "armory.3di", "shed.3di"]:
		_copy_fixture("threedi/synth/" + name, root.path_join("models").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return null


func _picture(device: SubViewport) -> Image:
	return device.get_texture().get_image()


func test_the_picture_shows_the_terrain() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	_open_mission()
	assert_true(_seam.open_document(MISSION_PATH))
	var state := await _await_ready(MISSION_PATH)
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device: SubViewport = _app.get_viewport_device(MISSION_PATH, "mission")
	assert_not_null(device)
	if device == null:
		return
	# Drawn by the Document's canvas and rendered: a few frames for the terrain's pages.
	for _frame in 30:
		await get_tree().process_frame
	var builds := int(_state(MISSION_PATH).get("builds", 0))
	var shown := _picture(device)
	assert_not_null(shown)
	assert_gt(shown.get_width(), 16)
	# Straight down over the target: the terrain fills the picture's middle.
	assert_true(_seam.done({"kind": "edit_in_viewport", "path": MISSION_PATH, "command": {"name": "top", "kind": "mission"}}))
	for _frame in 30:
		await get_tree().process_frame
	shown = _picture(device)
	var at := Vector2i(shown.get_width() / 2, shown.get_height() / 2)
	var with_terrain := shown.get_pixelv(at)
	assert_true(_seam.done({"kind": "set_viewport", "path": MISSION_PATH,
			"viewport": {"kind": "mission", "options": {"show": {"terrain": false}}}}))
	for _frame in 10:
		await get_tree().process_frame
	var without := _picture(device).get_pixelv(at)
	assert_false(with_terrain.is_equal_approx(without),
			"where the terrain lies the picture is the terrain's: %s with it, %s without" % [with_terrain, without])
	assert_eq(int(_state(MISSION_PATH).get("builds", 0)), builds, "a layer shown or not is an Update")


func test_a_mission_and_a_model_render_in_turn() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	_open_mission()
	# The model first (the Preview shows it), then the mission (the Document's main view): the Preview
	# keeps the model's pane while the mission is the active document.
	assert_true(_seam.open_document(MODEL_PATH))
	assert_eq(String((await _await_ready(MODEL_PATH)).get("status", "")), "ready")
	assert_true(_seam.open_document(MISSION_PATH))
	assert_eq(String((await _await_ready(MISSION_PATH)).get("status", "")), "ready")
	var mission: SubViewport = _app.get_viewport_device(MISSION_PATH, "mission")
	var model: SubViewport = _app.get_viewport_device(MODEL_PATH, "model")
	assert_not_null(mission)
	assert_not_null(model)
	if mission == null or model == null:
		return
	var waters := mission.find_children("*", "Water", true, false)
	var environments := mission.find_children("*", "MissionEnvironment", true, false)
	assert_false(waters.is_empty())
	assert_false(environments.is_empty())
	if waters.is_empty() or environments.is_empty():
		return
	var water := waters[0] as Water
	var environment := environments[0] as MissionEnvironment
	for _frame in 10:
		await get_tree().process_frame
	# Frame after frame: one of the two renders, in turn; the globals are the renderer's.
	var mission_frames := 0
	var model_frames := 0
	var last := ""
	for frame in 12:
		await get_tree().process_frame
		var mission_renders := mission.render_target_update_mode == SubViewport.UPDATE_ONCE
		var model_renders := model.render_target_update_mode == SubViewport.UPDATE_ONCE
		assert_true(mission_renders != model_renders,
				"frame %d: one device renders (mission %s, model %s)" % [frame, mission_renders, model_renders])
		var now := "mission" if mission_renders else "model"
		assert_ne(now, last, "frame %d: in turn" % frame)
		last = now
		if model_renders:
			model_frames += 1
			assert_false(environment.is_lighting_block_writer(),
					"frame %d: the model renders under the shipped defaults, no environment its writer" % frame)
			assert_false(water.is_mirror_enabled(), "frame %d: no mirror pass in a frame the mission does not present" % frame)
		else:
			mission_frames += 1
			assert_true(environment.is_lighting_block_writer(),
					"frame %d: the mission renders under its environment's block" % frame)
			assert_true(water.is_mirror_enabled(), "frame %d: the mirror pass in the mission's frame" % frame)
	assert_eq(mission_frames, 6)
	assert_eq(model_frames, 6)


## S14 review M1: a mission building beside a model in the Preview never lights the model. The model
## ready and rendering (its state, the shipped defaults, published once), the mission opened: no
## environment and no water writes a process-wide global while the mission builds (its environment,
## its water and its time set in its units; MissionEnvironment.get_global_writes,
## Water.get_global_writes: a game build's RenderingServer reads no global back), and the
## environment's lighting block is no writer's. Once built and drawn, the mission publishes its own.
func test_a_mission_building_never_lights_the_model() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	_app.build_budget_ms = 0
	_open_mission()
	assert_true(_seam.open_document(MODEL_PATH))
	assert_eq(String((await _await_ready(MODEL_PATH)).get("status", "")), "ready")
	for _frame in 5:
		await get_tree().process_frame
	var environment_writes := MissionEnvironment.get_global_writes()
	var water_writes := Water.get_global_writes()
	assert_true(_seam.open_document(MISSION_PATH))
	var loading := 0
	for frame in 900:
		await get_tree().process_frame
		var state := _state(MISSION_PATH)
		if String(state.get("status", "")) != "loading":
			if loading > 0:
				break
			continue
		loading += 1
		assert_eq(MissionEnvironment.get_global_writes(), environment_writes,
				"frame %d of the mission's build: no environment global written" % frame)
		assert_eq(Water.get_global_writes(), water_writes, "frame %d of the mission's build: no water global written" % frame)
	assert_gt(loading, 10, "the mission built over frames beside the model")
