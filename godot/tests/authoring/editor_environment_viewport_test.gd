extends GutTest

## The environment viewport's device headless through the editor's wire seam (ADR 0046 DI-19b): an
## environment opened draws its sky as the game draws it, through the runtime's own nodes in an offscreen
## SubViewport (MissionEnvironment, Weather, SkyDome, Celestial, Water, Terrain, Precipitation and the
## particle renderer's overlay passes), over the terrain of the mission that runs on it; the viewport's
## clock and weather reach the runtime's environment (a scrub its time, rain and overcast as a script sets
## them, the drops drawn), and an edit of the environment its picture.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION := "res://../fixtures/bms/synth_logic.bms"
const FIXTURES := "res://../fixtures/"
const ENV := "env/synth_full.env"
const TERRAIN_TEXTURES := ["mnml_c", "mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1", "mnml_t"]

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor environment viewport %d" % Time.get_ticks_usec())
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


func _copy_fixture(source: String, target: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FIXTURES + source))
	assert_false(bytes.is_empty(), "the fixture is available: " + source)
	_write(target, bytes)


## A small grey true-colour TGA (all channels alike).
static func _tga(size: int) -> PackedByteArray:
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


## A project holding the minted mission, its terrain (Tmap) and its environment (synth_full), the
## environment open.
func _open_environment() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor environment project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Environment Viewport"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join("missions").path_join("synth_logic.bms"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION)))
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"), _tga(32))
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return _seam.open_document(ENV)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": ENV, "kind": "environment", "limit": 50})


func _change(change: Dictionary) -> bool:
	var request := {"kind": "set_viewport", "path": ENV, "viewport": change.merged({"kind": "environment"})}
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return bool(answer.get("outcome", {}).get("done", false))


## The viewport ready on its device, a build newer than `builds` ended: a frame at a time.
func _await_built(builds := 0) -> Dictionary:
	var state := _state()
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > builds and not bool(state.get("device", {}).get("build", {}).get("loading", true)):
			break
		_app.pump()
		await get_tree().process_frame
		state = _state()
	_app.pump()
	return _state()


func _frames(count: int) -> void:
	for _frame in count:
		_app.pump()
		await get_tree().process_frame


func _device_node(type: String) -> Node:
	var device: SubViewport = _app.get_viewport_device(ENV, "environment")
	if device == null:
		return null
	var found := device.find_children("*", type, true, false)
	return found[0] if not found.is_empty() else null


func test_an_environment_draws_its_sky_over_its_missions_terrain() -> void:
	if _app == null:
		return
	assert_true(_open_environment(), "the environment opens")
	var state := await _await_built()
	assert_eq(String(state.get("kind", "")), "environment", str(state))
	assert_eq(String(state.get("status", "")), "ready", str(state.get("message", "")))
	var body: Dictionary = state.get("body", {})
	assert_eq(String(body.get("mission", {}).get("path", "")), "missions/synth_logic.bms", str(body.get("mission")))
	# The runtime's own nodes draw it.
	var environment := _device_node("MissionEnvironment") as MissionEnvironment
	var terrain := _device_node("Terrain") as Terrain
	for type in ["Weather", "SkyDome", "Celestial", "Water", "Precipitation", "ParticleRenderer", "Camera3D"]:
		assert_not_null(_device_node(type), "its device holds a " + type)
	# S23 C: the game's frame effects, FrameFx the terminal compositor (its bloom, the display decode) and the
	# sun-glare veil over the picture, the game's shader on it.
	assert_not_null(_device_node("FrameFx"), "its device holds the game's FrameFx")
	assert_null(_device_node("DisplayDecode"), "FrameFx is the one decode")
	var veil := _device_node("ColorRect") as ColorRect
	assert_not_null(veil, "the sun veil")
	if veil != null:
		assert_eq(String(veil.name), "SunVeil")
		var material := veil.material as ShaderMaterial
		assert_not_null(material, "the veil's shader")
		if material != null:
			assert_eq(material.shader.resource_path, "res://shaders/sun_veil_overlay.gdshader")
	assert_not_null(environment, "and the mission environment")
	assert_not_null(terrain, "and the terrain")
	if environment == null or terrain == null:
		return
	assert_true(environment.is_loaded(), "the environment loaded through the project's files")
	assert_not_null(terrain.get_terrain_data(), "the mission's terrain (Tmap) under it")
	assert_eq((body.get("missing", []) as Array).size(), 0, str(body.get("missing")))

	# A scrub: the runtime's clock at the viewport's time.
	assert_true(_change({"options": {"time": 6.5}, "clock": {"playing": false}}))
	await _frames(4)
	assert_eq(String(_state().get("body", {}).get("clock", {}).get("time", "")), "06:30")
	assert_almost_eq(environment.get_mission_minute_of_day(), 390.0, 0.5, "the environment's clock at 06:30")
	assert_true(_change({"options": {"time": 23.0}}))
	await _frames(4)
	assert_true(environment.is_night_phase(), "23:00 is night: the moon lights it")

	# Rain and overcast as a script sets them, the game's springs taking a 32nd of the way a tick: the clock
	# run fast until both stand at their targets, the drops drawn.
	assert_true(_change({"options": {"rain": {"percent": 100, "seconds": 0},
			"overcast": {"percent": 100, "seconds": 0}}, "clock": {"playing": true, "rate": 20}}))
	for _frame in 900:
		# The springs' last steps are a unit a tick: there at 0xFFFF, the start's clamp.
		if environment.get_rain_current() >= 0.99998 and environment.get_overcast_blend() >= 0.99998:
			break
		_app.pump()
		await get_tree().process_frame
	await _frames(2)
	assert_almost_eq(environment.get_rain_current(), 1.0, 0.01, "it rains in the runtime's environment")
	assert_almost_eq(environment.get_overcast_blend(), 1.0, 0.01, "overcast in the runtime's environment")
	assert_true(_change({"clock": {"rate": 1}}))
	body = _state().get("body", {})
	var rain: Dictionary = body.get("weather", {}).get("rain", {})
	assert_true(bool(rain.get("falling", false)), str(body.get("weather")))
	assert_eq(int(rain.get("drops", 0)), 3072, "every drop the game draws at full rain")

	# An edit of the environment: its picture made again over the document as Save would write it.
	var builds := int(_state().get("builds", 0))
	var row: int = _seam.get_row_id(0)
	assert_gt(row, 0, "the environment's row")
	var edit := {"kind": "edit_record", "path": ENV, "edits": [{"op": "set", "id": row, "field": "fog_level", "value": 300}]}
	var answer: Dictionary = _seam.request(edit)
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
	state = await _await_built(builds)
	assert_gt(int(state.get("builds", 0)), builds, "the device built again")
	await _frames(4)
	assert_almost_eq(environment.get_fog_level_target(), 300.0, 1.0, "the edited fog reached the runtime")

	# A terrain key added to the environment (D-TERRAIN-18): the terrain's reader takes the .env's lines after
	# the mission's .trn, so the picture's terrain loads again with the key over Tmap.trn's (its detail density,
	# 128), and the environment's uses say what it sets over.
	builds = int(_state().get("builds", 0))
	edit = {"kind": "edit_record", "path": ENV, "edits": [{"op": "add", "kind": "terrain_key", "parent": row, "as": "k"},
			{"op": "set", "id": "k", "field": "value", "value": "64"}]}
	answer = _seam.request(edit)
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
	state = await _await_built(builds)
	assert_gt(int(state.get("builds", 0)), builds, "the device built again")
	terrain = _device_node("Terrain") as Terrain
	assert_not_null(terrain, "the terrain drawn again")
	if terrain != null and terrain.get_terrain_data() != null:
		assert_eq(terrain.get_terrain_data().get_detail_density(), 64, "the environment's key over Tmap.trn's 128")
	var uses: Dictionary = _seam.query("environment_uses", {"path": ENV})
	var missions: Array = uses.get("missions", [])
	assert_eq(missions.size(), 1, str(uses))
	if missions.size() == 1:
		var keys: Array = missions[0].get("terrain_keys", [])
		assert_eq(keys.size(), 1, str(missions[0]))
		if keys.size() == 1:
			assert_eq(String(keys[0].get("key", "")), "polytrn_detaildensity")
			assert_eq(String(keys[0].get("value", "")), "64")
			assert_eq(String(keys[0].get("over", "")), "128", "over Tmap.trn's own")
			assert_eq(String(keys[0].get("over_file", "")), "terrain/Tmap.trn")
