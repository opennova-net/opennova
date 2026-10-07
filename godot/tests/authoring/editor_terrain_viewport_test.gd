extends GutTest

## The terrain viewport's device headless through the editor's wire seam (ADR 0046 DI-30b): a terrain opened is
## drawn on its own, with no mission, through the runtime's own nodes in an offscreen SubViewport (Terrain over
## TerrainData, Water, FoliageDispatcher with the terrain's definitions configured, MissionEnvironment and SkyDome
## under the environment of the mission that runs on it, or the engine's own); the ground overlay the options ask
## tints the terrain as an Update; an edit of the terrain builds its picture again.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION := "res://../fixtures/bms/synth_logic.bms"
const FIXTURES := "res://../fixtures/"
const TERRAIN := "terrain/Tmap.trn"
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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor terrain viewport %d" % Time.get_ticks_usec())
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


## A project holding the minted mission, its terrain (Tmap, its two foliage models: the synth crate under both
## names) and its environment (synth_full), the terrain open.
func _open_terrain() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor terrain project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Terrain Viewport"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join("missions").path_join("synth_logic.bms"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION)))
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"), _tga(32))
	for name in ["bush1.3di", "bush2.3di"]:
		_copy_fixture("threedi/synth/crate.3di", root.path_join("models").path_join(name))
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return _seam.open_document(TERRAIN)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": TERRAIN, "kind": "terrain", "limit": 50})


func _change(change: Dictionary) -> bool:
	var request := {"kind": "set_viewport", "path": TERRAIN, "viewport": change.merged({"kind": "terrain"})}
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
	var device: SubViewport = _app.get_viewport_device(TERRAIN, "terrain")
	if device == null:
		return null
	var found := device.find_children("*", type, true, false)
	return found[0] if not found.is_empty() else null


func test_a_terrain_draws_on_its_own_under_its_missions_environment() -> void:
	if _app == null:
		return
	assert_true(_open_terrain(), "the terrain opens")
	var state := await _await_built()
	assert_eq(String(state.get("kind", "")), "terrain", str(state))
	assert_eq(String(state.get("status", "")), "ready", str(state.get("message", "")))
	var body: Dictionary = state.get("body", {})
	assert_eq(String(body.get("mission", {}).get("path", "")), "missions/synth_logic.bms", str(body.get("mission")))
	# The runtime's own nodes draw it.
	var environment := _device_node("MissionEnvironment") as MissionEnvironment
	var terrain := _device_node("Terrain") as Terrain
	var foliage := _device_node("FoliageDispatcher") as FoliageDispatcher
	for type in ["Weather", "SkyDome", "Water", "Camera3D"]:
		assert_not_null(_device_node(type), "its device holds a " + type)
	assert_not_null(environment, "and the mission environment")
	assert_not_null(terrain, "and the terrain")
	assert_not_null(foliage, "and the foliage")
	if environment == null or terrain == null or foliage == null:
		return
	assert_true(environment.is_loaded(), "the mission's environment loaded through the project's files")
	assert_not_null(terrain.get_terrain_data(), "the terrain loaded through the project's files")
	assert_eq((body.get("missing", []) as Array).size(), 0, str(body.get("missing")))
	# Its foliage configured from its two definitions, as the game's load configures it.
	var enabled := 0
	for diagnostic in foliage.get_slot_diagnostics():
		enabled += 1 if String((diagnostic as Dictionary).get("status", "")) == "enabled" else 0
	assert_eq(enabled, 2, "both definitions draw: " + str(foliage.get_slot_diagnostics()))

	# The engine's own environment: no mission, the picture built again with no environment file.
	assert_true(_change({"options": {"mission": "none"}}))
	state = await _await_built(int(state.get("builds", 0)))
	assert_eq(state.get("body", {}).get("mission"), null, str(state.get("body", {}).get("mission")))
	assert_false(environment.is_loaded(), "no .env read: the engine's own environment")

	# The ground overlay: an Update tinting the terrain, asked away gone.
	var builds := int(state.get("builds", 0))
	assert_false(terrain.has_ground_overlay(), "no overlay asked")
	assert_true(_change({"options": {"overlay": "surfaces"}}))
	await _frames(3)
	state = _state()
	assert_eq(String(state.get("body", {}).get("overlay", {}).get("kind", "")), "surfaces")
	assert_true(terrain.has_ground_overlay(), "the terrain tinted")
	assert_true(_change({"options": {"overlay": "none"}}))
	await _frames(3)
	assert_false(terrain.has_ground_overlay(), "asked away, gone")
	assert_eq(int(_state().get("builds", 0)), builds, "an overlay is an Update, never a build")

	# An edit of the terrain: its picture made again over the document as Save would write it (the water, with no
	# mission's header over it, the terrain's).
	var row: int = _seam.get_row_id(0)
	assert_gt(row, 0, "the terrain's row")
	var edit := {"kind": "edit_record", "path": TERRAIN, "edits": [{"op": "set", "id": row, "field": "water_height", "value": 80}]}
	var answer: Dictionary = _seam.request(edit)
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
	state = await _await_built(builds)
	assert_gt(int(state.get("builds", 0)), builds, "the device built again")
	await _frames(4)
	assert_almost_eq(float(state.get("body", {}).get("ground", {}).get("water_height", 0.0)), 40.0, 0.01,
			"the edited water height, in metres")
	var water := _device_node("Water") as Water
	assert_not_null(water)
	if water != null:
		assert_almost_eq(water.get_water_height(), 40.0, 0.01, "the edited water reached the runtime's water")
