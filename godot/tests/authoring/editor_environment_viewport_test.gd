extends GutTest

## The environment viewport's device headless through the editor's wire seam (ADR 0046 DI-19b): an
## environment opened draws its sky as the game draws it, through the runtime's own nodes in an offscreen
## SubViewport (MissionEnvironment, Weather, SkyDome, Celestial, Water, Terrain, Precipitation and the
## particle renderer's overlay passes), over the terrain of the mission that runs on it; the viewport's
## clock and weather reach the runtime's environment (a scrub its time, rain and overcast as a script sets
## them, the drops drawn), and an edit of the environment its picture.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const ENV := "env/synth_full.env"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "environment viewport", _dirs)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


## A project holding the minted mission, its terrain (Tmap, its two foliage definitions' models)
## and its environment (synth_full), the environment open.
func _open_environment() -> bool:
	var root := EditorFixture.new_project(self, _seam, _dirs, "Environment Viewport")
	EditorFixture.mint_tmap_mission(self, _seam, root, {"small": true, "foliage": true})
	return _seam.open_document(ENV)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": ENV, "kind": "environment", "limit": 50})


func _change(change: Dictionary) -> bool:
	var request := {"kind": "set_viewport", "path": ENV, "viewport": change.merged({"kind": "environment"})}
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return bool(answer.get("outcome", {}).get("done", false))


## The viewport ready on its device, a build newer than `builds` ended (EditorSeam.await_viewport).
func _await_built(builds := 0) -> Dictionary:
	return await _seam.await_viewport({"path": ENV, "kind": "environment", "limit": 50}, builds)


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
	# S23 C: the terrain's foliage beside it, configured as the game's load configures it: Tmap's two definitions.
	var foliage := _device_node("FoliageDispatcher") as FoliageDispatcher
	assert_not_null(foliage, "the game's foliage beside the terrain")
	if foliage != null:
		var enabled := 0
		for diagnostic: Dictionary in foliage.get_slot_diagnostics():
			enabled += 1 if String(diagnostic.get("status", "")) == "enabled" else 0
		assert_eq(enabled, 2, "Tmap's two foliage definitions configured: %s" % str(foliage.get_slot_diagnostics()))
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
