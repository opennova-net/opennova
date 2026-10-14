extends GutTest

## A sculpt shown in the mission's 3D view without a rebuild (ADR 0046 S25, the heights lane: the device hook), headless
## over the editor-enabled GDExtension: a Raise stroke in the mission's view is an Update of its picture (its builds
## stand, the device's Terrain node and its data stay the ones it built), the device's terrain data answers the
## raised ground at once (the stroke's regions patched into it, Terrain.refresh_region over them), the bake's landing
## puts its tiles in place, and the mission's Undos put the shipped ground back, the device following each with no
## build.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const MISSION := "missions/synth_logic.bms"
# Mission (-200, 200): the Tmap terrain's sector at grid (3, 3), quadrant 1, the atlas texel (312, 312).
const AT := Vector2(-200.0, 200.0)

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null
var _gesture := 0


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "heights device", _dirs)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


## A new project holding the minted mission on the Tmap terrain (its .trn, heights, maps and the textures it names)
## and its environment, scanned, the mission open.
func _open_mission() -> bool:
	var root := EditorFixture.new_project(self, _seam, _dirs, "Heights Device")
	EditorFixture.mint_tmap_mission(self, _seam, root)
	return _seam.open_document(MISSION)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "limit": 10, "path": MISSION})


func _await_ready() -> Dictionary:
	var state := _state()
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		await get_tree().process_frame
		state = _state()
	_app.pump()
	return _state()


func _terrain(state: Dictionary) -> Terrain:
	var device: SubViewport = _app.get_viewport_device(String(state.get("path", "")), String(state.get("kind", "")))
	if device == null:
		return null
	var found := device.find_children("*", "Terrain", true, false)
	return found[0] as Terrain if not found.is_empty() else null


## The mission's ground as the device's terrain data answers it: the mission height at (x, y).
func _ground(data: TerrainData, at: Vector2) -> float:
	var godot := MissionObjectPlacer.bms_to_godot_position(Vector3(at.x, at.y, 0.0))
	var height := data.get_height_world_bilinear(godot)
	return MissionObjectPlacer.godot_to_bms_position(Vector3(godot.x, height, godot.z)).z


## A sample of one stroke: gesture 0 begins it, its answer's token carries the next (a wire stroke's token must be
## open); a sample but the last is not settled (its gesture holds the validation back: a settle would wait out its
## lapse).
func _stroke(points: Array, end: bool) -> Dictionary:
	var fields := {"kind": "terrain_stroke", "path": MISSION, "stroke": {"plane": "heights",
			"brush": {"tool": "raise", "radius": 8, "strength": 1}, "points": points, "gesture": _gesture, "end": end}}
	var answer: Dictionary = _seam.request(fields) if end else _seam.sample(fields)
	_gesture = 0 if end else int(answer.get("outcome", {}).get("gesture", 0))
	_app.pump()
	return answer


func test_a_sculpt_is_an_update_of_the_missions_view() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var builds := int(state.get("builds", 0))
	var terrain := _terrain(state)
	assert_not_null(terrain, "the device built its terrain")
	if terrain == null:
		return
	var data := terrain.get_terrain_data()
	var before := _ground(data, AT)

	# The stroke's first dab: an Update, the device's ground raised where it fell, nothing built again.
	var first := _stroke([[AT.x, AT.y]], false)
	assert_true(bool(first.get("outcome", {}).get("done", false)), str(first.get("outcome")))
	_app.pump()
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a stroke is an Update: nothing builds again")
	assert_eq(_terrain(state), terrain, "the device's terrain stands")
	assert_eq(terrain.get_terrain_data(), data, "with its data")
	assert_gt(_ground(data, AT), before + 0.5, "the device's ground raised under the dab")

	# The stroke ends; its bake lands in the background: its tiles put in place, still nothing built.
	var last := _stroke([[AT.x + 4.0, AT.y]], true)
	assert_true(bool(last.get("outcome", {}).get("done", false)), str(last.get("outcome")))
	# The seed and the region bake run on the bake's threads: a deadline in time, not frames (a CI runner's
	# few cores take far longer than a desktop's).
	var landed := false
	var deadline := Time.get_ticks_msec() + 180_000
	while Time.get_ticks_msec() < deadline:
		_app.pump()
		await get_tree().process_frame
		var heights: Dictionary = _seam.query("terrain", {"op": "bake", "path": MISSION})
		if not heights.has("error") and not bool(heights.get("due", true)):
			landed = true
			break
	assert_true(landed, "the bake landed")
	_app.pump()
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "the landing is an Update")
	assert_gt(_ground(data, AT), before + 0.5)

	# The mission's Undos, each an Update the device follows: its ground the document's after each (the view's other
	# requests between the stroke's samples may have ended its gesture: a step each), the shipped ground at the last.
	for _undo in 3:
		var undone: Dictionary = _seam.request({"kind": "undo", "path": MISSION})
		if not bool(undone.get("outcome", {}).get("done", false)):
			break
		var wanted := 0.0
		for _frame in 120:
			_app.pump()
			await get_tree().process_frame
			wanted = float(_seam.query("terrain", {"op": "sample", "path": MISSION, "x": AT.x, "y": AT.y}).get("height", 0.0))
			var bake: Dictionary = _seam.query("terrain", {"op": "bake", "path": MISSION})
			if not bool(bake.get("due", true)) and absf(_ground(data, AT) - wanted) < 0.001:
				break
		assert_almost_eq(_ground(data, AT), wanted, 0.001, "the device's ground the document's after an undo")
		assert_eq(int(_state().get("builds", 0)), builds, "the undo is an Update")
		if absf(wanted - before) < 0.001:
			break
	assert_almost_eq(_ground(data, AT), before, 0.001, "the shipped ground back")


## The device's ground under the camera's ray, where the brush meets it with a picture (ViewportDevice::surface_between):
## each point the mission view's hit answers over the terrain lies on its pixel's ray and at the game's own height there
## (the terrain query's sample), the crossing the brush's headless space finds with no device for the same pixel
## (editor_terrain_family's brush-space leg), so a stroke over the wire lands where the ring shows.
func test_the_devices_ground_is_the_games_under_the_ray() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device: SubViewport = _app.get_viewport_device(MISSION, "mission")
	assert_not_null(device, "the mission's device")
	if device == null:
		return
	var camera := device.get_camera_3d()
	assert_not_null(camera, "its camera")
	if camera == null:
		return
	var width := float(state.get("device", {}).get("width", 0))
	var height := float(state.get("device", {}).get("height", 0))
	assert_gt(width, 0.0, str(state.get("device")))
	var scale := Vector2(device.size) / Vector2(maxf(width, 1.0), maxf(height, 1.0))
	var met := 0
	for row in range(1, 6):
		for col in range(1, 6):
			# Off the column over the picture's middle (x = 0, a sector seam: editor_terrain_family's note).
			var pixel := Vector2(width * col / 6.0 + 3.0, height * row / 6.0)
			var hit: Dictionary = _seam.query("viewport", {"op": "hit", "path": MISSION, "kind": "mission",
					"x": pixel.x, "y": pixel.y})
			var ground: Dictionary = hit.get("ground", {}) if hit.get("ground") is Dictionary else {}
			if String(ground.get("on", "")) != "terrain":
				continue
			var at: Array = ground.get("at", [])
			var point := Vector3(float(at[0]), float(at[1]), float(at[2]))
			var sample: Dictionary = _seam.query("terrain", {"op": "sample", "path": MISSION, "x": point.x, "y": point.y})
			assert_almost_eq(point.z, float(sample.get("height", INF)), 0.01, "the device's ground at %s the game's height" % str(point))
			var shown := camera.unproject_position(MissionObjectPlacer.bms_to_godot_position(point)) / scale
			assert_lt(shown.distance_to(pixel), 2.0, "the device's ground at %s on the ray of %s (shown at %s)" % [str(point), str(pixel), str(shown)])
			met += 1
	assert_gt(met, 0, "the camera's rays met the terrain")
