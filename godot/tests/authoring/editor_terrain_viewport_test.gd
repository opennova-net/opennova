extends GutTest

## The terrain viewport's device headless through the editor's wire seam (ADR 0046 DI-30b): a terrain opened is
## drawn on its own, with no mission, through the runtime's own nodes in an offscreen SubViewport (Terrain over
## TerrainData, Water, FoliageDispatcher with the terrain's definitions configured, MissionEnvironment and SkyDome
## under the environment of the mission that runs on it, or the engine's own); the ground overlay the options ask
## tints the terrain as an Update; an edit of the terrain builds its picture again.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const TERRAIN := "terrain/Tmap.trn"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "terrain viewport", _dirs)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


## A project holding the minted mission, its terrain (Tmap, its two foliage models: the synth crate under both
## names) and its environment (synth_full), the terrain open.
func _open_terrain() -> bool:
	var root := EditorFixture.new_project(self, _seam, _dirs, "Terrain Viewport")
	EditorFixture.mint_tmap_mission(self, _seam, root, {"small": true, "foliage": true})
	return _seam.open_document(TERRAIN)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": TERRAIN, "kind": "terrain", "limit": 50})


func _change(change: Dictionary) -> bool:
	var request := {"kind": "set_viewport", "path": TERRAIN, "viewport": change.merged({"kind": "terrain"})}
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return bool(answer.get("outcome", {}).get("done", false))


## The viewport ready on its device, a build newer than `builds` ended (EditorSeam.await_viewport).
func _await_built(builds := 0) -> Dictionary:
	return await _seam.await_viewport({"path": TERRAIN, "kind": "terrain", "limit": 50}, builds)


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

	# The terrain's water keywords under the mission's .env (env-tod-re.md #43): the game's load reads the .trn
	# first, so the terrain's murk is the picture's until a .env line of it; synth_full.env writes one (0.8).
	var row: int = _seam.get_row_id(0)
	assert_gt(row, 0, "the terrain's row")
	var murk_edit := {"kind": "edit_record", "path": TERRAIN, "edits": [{"op": "set", "id": row, "field": "water_murk", "value": 0.3}]}
	var murk_answer: Dictionary = _seam.request(murk_edit)
	assert_true(bool(murk_answer.get("outcome", {}).get("done", false)), str(murk_answer))
	state = await _await_built(int(state.get("builds", 0)))
	assert_almost_eq(float(environment.get_environment_data().get_water_murk()), 0.8, 0.0001,
			"the mission's .env writes the murk after the terrain's")

	# The engine's own environment: no mission, the picture built again with no environment file, under the
	# terrain's own lines (the game's load reads the .trn ahead of a .env it lacks): its murk is the water's.
	assert_true(_change({"options": {"mission": "none"}}))
	state = await _await_built(int(state.get("builds", 0)))
	assert_eq(state.get("body", {}).get("mission"), null, str(state.get("body", {}).get("mission")))
	assert_true(environment.is_loaded(), "the engine's own environment, loaded as the game loads it")
	assert_eq(String(environment.get_environment_data().get_source_path()), "", "no .env read")
	assert_almost_eq(float(environment.get_environment_data().get_water_murk()), 0.3, 0.0001,
			"the terrain's murk with no .env over it")

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
