extends GutTest

## The mission viewport headless through the editor's wire seam (ADR 0046 S14): a mission opened in a
## minted project shows in its Main-role viewport (kind "mission"), read through query_json's
## `viewport` and changed through request_json's set_viewport and edit_in_viewport; its device (the
## mission's Environment, Sky, Water, Terrain and Camera3D in an offscreen SubViewport,
## get_viewport_device) builds its picture over the Shell's frames (loading before ready; a unit a
## frame at a budget of 0, the labels in the build's order: environment, terrain files, terrain, sky,
## water, pose) from the project's own files (the minted terrain, its textures, the env, items.def)
## and takes what changed at the next pump. The marks sit where the device's camera draws them: each
## entity's `screen` is within half a pixel of Camera3D.unproject_position of
## MissionObjectPlacer.bms_to_godot_position of its position, and the yaw handle of a yaw-90 entity
## lies east of its anchor; the camera set on the wire (a compass heading) is the device's; an
## entity's move is an Update (the build generation stands); a hit at a mark's pixel names its
## record and a box the records inside. The ground is the terrain's: a move with `stick` keeps the
## entity's height over TerrainData.get_height_world_bilinear. A file the mission names that the
## project lacks is a note, the picture standing without it; the mission kind keeps two devices,
## the third mission giving up the least recently used mission's (D3).

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION := "res://../fixtures/bms/synth_logic.bms"
const FIXTURES := "res://../fixtures/"
## The terrain's texture slots Tmap.trn names (fixtures/terrain/tmap), minted as the runtime fixture
## mints them (godot/tests/support/runtime_fixture.gd): the colormap and the blend map 2048 a side.
const TERRAIN_TEXTURES := ["mnml_c", "mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1", "mnml_t"]

## The minted textures' bytes, made once a run.
static var _textures: Dictionary = {}

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor mission viewport %d" % Time.get_ticks_usec())
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


func _viewport(op: String, args := {}, path := "") -> Dictionary:
	var params: Dictionary = args.duplicate()
	params["op"] = op
	if not path.is_empty():
		params["path"] = path
	return _seam.query("viewport", params)


func _state(path := "") -> Dictionary:
	return _viewport("state", {"limit": 200}, path)


func _ask(fields: Dictionary, path := "") -> Dictionary:
	var request: Dictionary = fields.duplicate()
	if not path.is_empty():
		request["path"] = path
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return answer


func _change(change: Dictionary, path := "") -> bool:
	return bool(_ask({"kind": "set_viewport", "viewport": change}, path).get("outcome", {}).get("done", false))


func _await_ready(path := "") -> Dictionary:
	var state := _state(path)
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		await get_tree().process_frame
		state = _state(path)
	# What the device reports of its built picture (the files it missed, its ground) reaches the
	# viewport at the pump after the build's last unit.
	_app.pump()
	return _state(path)


func _device(state: Dictionary) -> SubViewport:
	return _app.get_viewport_device(String(state.get("path", "")), String(state.get("kind", "")))


func _device_node(state: Dictionary, type: String) -> Node:
	var device := _device(state)
	if device == null:
		return null
	var found := device.find_children("*", type, true, false)
	return found[0] if not found.is_empty() else null


## The device's own camera, "Camera" under its Mission root (the water's mirror has a camera of its
## own under it).
func _camera(state: Dictionary) -> Camera3D:
	var device := _device(state)
	if device == null:
		return null
	var found := device.find_children("Camera", "Camera3D", true, false)
	return found[0] as Camera3D if not found.is_empty() else null


func _write(path: String, bytes: PackedByteArray) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "wrote %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


func _vector(values: Variant) -> Vector3:
	var list: Array = values if values is Array else [0, 0, 0]
	return Vector3(float(list[0]), float(list[1]), float(list[2]))


## A texture `size` a side, grey, as an uncompressed true-colour TGA (all channels alike, so RGBA
## bytes are BGRA TGA bytes): the runtime fixture's minting.
static func _tga(size: int) -> PackedByteArray:
	if _textures.has(size):
		return _textures[size]
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.5, 0.5, 0.5, 1.0))
	var header := PackedByteArray()
	header.resize(18)
	header[2] = 2 # Uncompressed true-color TGA.
	header.encode_u16(12, size)
	header.encode_u16(14, size)
	header[16] = 32
	header[17] = 0x28 # Top-origin, eight alpha bits.
	_textures[size] = header + image.get_data()
	return _textures[size]


func _copy_fixture(source: String, target: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FIXTURES + source))
	assert_false(bytes.is_empty(), "the fixture is available: " + source)
	_write(target, bytes)


## The files the mission's picture reads, written into the project at `root`: the terrain (Tmap.trn,
## its heights, its two maps and the textures it names), the environment (synth_full.env and its
## clouds), the item table the mission's items are in, and the models their graphics name.
func _mint_project(root: String) -> void:
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"), _tga(2048 if name == "mnml_c" or name == "mnml_d1" else 32))
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	_copy_fixture("def/items.def", root.path_join("defs").path_join("items.def"))
	for name in ["pump.3di", "armory.3di", "shed.3di"]:
		_copy_fixture("threedi/synth/" + name, root.path_join("models").path_join(name))


## A new project holding the minted mission as missions/synth_logic.bms (and, `whole`, the files its
## picture reads), scanned, the mission open.
func _open_mission(whole := true, also: PackedStringArray = PackedStringArray()) -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor mission project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Mission Viewport Game"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	var mission := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION))
	_write(root.path_join("missions").path_join("synth_logic.bms"), mission)
	for name in also:
		_write(root.path_join("missions").path_join(name), mission)
	if whole:
		_mint_project(root)
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return _seam.open_document("missions/synth_logic.bms")


## The mission's ground, as the device's terrain data answers it: the mission height at (x, y).
func _ground(data: TerrainData, x: float, y: float) -> float:
	var at := MissionObjectPlacer.bms_to_godot_position(Vector3(x, y, 0.0))
	var height := data.get_height_world_bilinear(at)
	return MissionObjectPlacer.godot_to_bms_position(Vector3(at.x, height, at.z)).z


## The first item's mark of `state` (empty: none).
func _first_item(state: Dictionary) -> Dictionary:
	for row: Variant in state.get("items", []):
		if String((row as Dictionary).get("kind", "")) == "item":
			return row
	return {}


## The mark of record `id` in `state` (empty: none).
func _mark_of(state: Dictionary, id: int) -> Dictionary:
	for row: Variant in state.get("items", []):
		if int((row as Dictionary).get("id", 0)) == id:
			return row
	return {}


func test_the_mission_shows_in_its_viewport_through_its_device() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("kind", "")), "mission", str(state))
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_true(bool(state.get("current", false)))
	var counts: Dictionary = state.get("body", {}).get("counts", {})
	assert_eq(int(counts.get("items", 0)), 3)
	assert_eq(int(counts.get("buildings", 0)), 2)
	assert_eq(int(counts.get("markers", 0)), 5)
	assert_eq(int(counts.get("organics", 0)), 2)
	assert_eq(int(counts.get("areas", 0)), 2)
	assert_eq(int(counts.get("paths", 0)), 1, "the fixture's one used path")
	assert_eq(state.get("items", []).size(), 14, "a mark per entity and area")
	# The device: the mission's nodes, the camera where the viewport's is.
	var device := _device(state)
	assert_not_null(device, "the mission's device")
	assert_not_null(_device_node(state, "MissionEnvironment"))
	var camera := _camera(state)
	assert_not_null(camera)
	if camera == null:
		return
	var wire: Dictionary = state.get("camera", {})
	var eye := MissionObjectPlacer.bms_to_godot_position(_vector(wire.get("eye")))
	assert_true(camera.global_position.is_equal_approx(eye), "the device's camera at the viewport's eye")
	# Each entity's mark where the camera projects its position.
	var projected := 0
	for item: Variant in state.get("items", []):
		var mark: Dictionary = item
		if not mark.has("screen") or String(mark.get("kind", "")) == "area":
			continue
		var at := camera.unproject_position(MissionObjectPlacer.bms_to_godot_position(_vector(mark.get("at"))))
		assert_almost_eq(at.x, float(mark["screen"][0]), 0.5, "mark %s x" % mark.get("name"))
		assert_almost_eq(at.y, float(mark["screen"][1]), 0.5, "mark %s y" % mark.get("name"))
		projected += 1
	assert_gt(projected, 0, "an entity on the picture")


func test_the_camera_on_the_wire_and_a_move_as_an_update() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	# A move of the first item (the first framing shows every entity): an Update, the build
	# generation standing; the item's mark moves.
	var builds := int(state.get("builds", 0))
	var item: Dictionary = {}
	for row: Variant in state.get("items", []):
		if String((row as Dictionary).get("kind", "")) == "item":
			item = row
			break
	assert_false(item.is_empty())
	if item.is_empty():
		return
	var before := _vector(item.get("at"))
	var moved: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": int(item["id"]), "handle": "move", "by": [40, 0], "kind": "mission"}})
	assert_true(bool(moved.get("outcome", {}).get("done", false)), str(moved))
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a move is an Update: nothing builds again")
	var after := Vector3.ZERO
	for row: Variant in state.get("items", []):
		if int((row as Dictionary).get("id", 0)) == int(item["id"]):
			after = _vector((row as Dictionary).get("at"))
	assert_false(after.is_equal_approx(before), "the item moved")
	_seam.undo()
	_app.pump()
	state = _state()
	for row: Variant in state.get("items", []):
		if int((row as Dictionary).get("id", 0)) == int(item["id"]):
			assert_true(_vector((row as Dictionary).get("at")).is_equal_approx(before), "undone in one step")
	# A hit at a mark's pixel names its record; a box over the picture names every shown mark.
	var hits := 0
	for row: Variant in state.get("items", []):
		var mark: Dictionary = row
		if not mark.has("screen") or not bool(mark.get("shown", false)):
			continue
		var hit := _viewport("hit", {"x": float(mark["screen"][0]), "y": float(mark["screen"][1])})
		assert_true(bool(hit.get("current", false)) and int(hit.get("id", 0)) != 0, str(hit))
		hits += 1
		break
	assert_eq(hits, 1, "a shown mark on the picture")
	var size: Dictionary = state.get("device", {})
	var box := _viewport("box", {"x": 0.0, "y": 0.0, "x2": float(size.get("width", 1024)), "y2": float(size.get("height", 768))})
	assert_gt(int(box.get("count", 0)), 0, str(box))
	assert_eq(box.get("records", []).size(), int(box.get("count", 0)))
	# The camera set as a compass heading: east. The device's camera looks east (+x in Godot).
	assert_true(_change({"kind": "mission", "camera": {"yaw": 90, "pitch": 0, "distance": 50}}))
	state = _state()
	assert_almost_eq(float(state.get("camera", {}).get("yaw", -1)), 90.0, 0.001)
	var camera := _camera(state)
	assert_not_null(camera)
	if camera != null:
		var forward: Vector3 = -camera.global_transform.basis.z
		assert_almost_eq(forward.x, 1.0, 0.001, "yaw 90 looks east")
	# A refused member names itself.
	var refused: Dictionary = _ask({"kind": "set_viewport", "viewport": {"kind": "mission", "camera": {"eye": [0, 0, 0]}}})
	assert_false(bool(refused.get("outcome", {}).get("done", true)))
	# The frame command: the camera over every entity again, the marks back on the picture.
	assert_true(bool(_ask({"kind": "edit_in_viewport", "command": {"name": "frame", "kind": "mission"}}).get("outcome", {}).get("done", false)))
	state = _state()
	var shown := 0
	for row: Variant in state.get("items", []):
		shown += 1 if bool((row as Dictionary).get("shown", false)) else 0
	assert_gt(shown, 0, "framed: marks on the picture again")


## S14 V8: the mission builds over the Shell's frames. Opened at a budget of 0, its viewport is
## `loading` at the pump that takes it, one unit a frame, its progress never going back, the units'
## labels in the build's order (the environment, the terrain's files, the terrain a tile a step, the
## sky, the water, the pose), then `ready` with the layers under its device, the terrain built (the
## body's `ground`) and no file missing.
func test_the_mission_builds_over_frames() -> void:
	if _app == null:
		return
	_app.build_budget_ms = 0
	assert_true(_open_mission())
	_app.pump()
	var state := _state()
	assert_eq(String(state.get("status", "")), "loading", str(state))
	var total := int(state.get("progress", {}).get("total", 0))
	assert_gt(total, 6, "a unit each for the environment, the terrain's files and tiles, the sky, the water, the pose")
	var labels: PackedStringArray = []
	var done := 0
	var frames := 0
	while String(state.get("status", "")) == "loading" and frames < 600:
		var label := String(state.get("progress", {}).get("label", ""))
		if labels.is_empty() or labels[labels.size() - 1] != label:
			labels.append(label)
		var now := int(state.get("progress", {}).get("done", 0))
		assert_true(now >= done and now - done <= 1, "one unit a frame, never back: %d after %d" % [now, done])
		done = now
		await get_tree().process_frame
		frames += 1
		state = _state()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_eq(labels, PackedStringArray(["environment", "terrain files", "terrain", "sky", "water", "pose"]))
	var build: Dictionary = state.get("device", {}).get("build", {})
	gut.p("the mission at 0 ms a frame: %d frames awaited, its build %s" % [frames, str(build)])
	assert_eq(int(build.get("done", 0)), int(build.get("total", 0)), str(build))
	assert_gte(int(build.get("total", 0)), total, "the terrain's tiles counted as its build began")
	assert_eq(int(state.get("builds", 0)), 1)
	for type in ["Terrain", "MissionEnvironment", "SkyDome", "Water", "Camera3D"]:
		assert_not_null(_device_node(state, type), type + " under the device")
	assert_true(bool(state.get("body", {}).get("ground", false)), "the terrain built: a surface a ray lands on")
	assert_eq(int(state.get("body", {}).get("missing", -1)), 0, str(state.get("notes", [])))
	var terrain: Terrain = _device_node(state, "Terrain")
	assert_not_null(terrain.get_terrain_data(), "the terrain holds its data")
	# A pump with nothing changed builds nothing again; the time option is an Update.
	_app.pump()
	state = _state()
	assert_eq(int(state.get("builds", 0)), 1)
	assert_true(_change({"kind": "mission", "options": {"time": 6.5}}))
	state = _state()
	assert_eq(int(state.get("builds", 0)), 1, "the time of day is an Update")
	assert_eq(String(state.get("status", "")), "ready")


## S14 V8: the ground is the terrain's. A move of the first item with `stick` (the default) keeps its
## height over TerrainData.get_height_world_bilinear at the new place; the build generation stands;
## a vertical ray through the moved item meets the terrain where the height says.
func test_the_ground() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_true(bool(state.get("body", {}).get("ground", false)))
	var terrain: Terrain = _device_node(state, "Terrain")
	assert_not_null(terrain)
	if terrain == null:
		return
	var data: TerrainData = terrain.get_terrain_data()
	assert_not_null(data)
	if data == null:
		return
	var item := _first_item(state)
	assert_false(item.is_empty())
	if item.is_empty():
		return
	var before := _vector(item.get("at"))
	var clearance := before.z - _ground(data, before.x, before.y)
	var builds := int(state.get("builds", 0))
	var moved: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": int(item["id"]), "handle": "move", "by": [40, 0], "kind": "mission"}})
	assert_true(bool(moved.get("outcome", {}).get("done", false)), str(moved))
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a move is an Update")
	var after := _vector(_mark_of(state, int(item["id"])).get("at"))
	assert_false(after.is_equal_approx(before), "the item moved")
	var ground := _ground(data, after.x, after.y)
	assert_almost_eq(after.z, ground + clearance, 0.002, "stick: its height over the ground kept (16.16)")
	# The terrain under it, as a ray finds it.
	var top := MissionObjectPlacer.bms_to_godot_position(Vector3(after.x, after.y, ground + 500.0))
	var bottom := MissionObjectPlacer.bms_to_godot_position(Vector3(after.x, after.y, ground - 500.0))
	var hit := data.raycast_terrain(top, bottom)
	assert_true(hit.is_finite(), "the ray meets the terrain")
	if hit.is_finite():
		assert_almost_eq(MissionObjectPlacer.godot_to_bms_position(hit).z, ground, 0.05, "where the height says")
	# Stick off: a move leaves the height.
	assert_true(_change({"kind": "mission", "options": {"stick": false}}))
	var lifted: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": int(item["id"]), "handle": "move", "by": [0, 20], "kind": "mission"}})
	assert_true(bool(lifted.get("outcome", {}).get("done", false)), str(lifted))
	var still := _vector(_mark_of(_state(), int(item["id"])).get("at"))
	assert_almost_eq(still.z, after.z, 0.0001, "stick off: the height stands")


## S14 V8: a file the mission names that the project lacks is a note. A project with the mission alone
## (no terrain, no env): the viewport is ready all the same, its picture the environment's noon over
## no terrain (no ground), the terrain's .trn and the env among its notes.
func test_a_missing_file_is_a_note() -> void:
	if _app == null:
		return
	assert_true(_open_mission(false))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_false(bool(state.get("body", {}).get("ground", true)), "no terrain: no ground")
	var names: PackedStringArray = []
	for note: Variant in state.get("notes", []):
		if String((note as Dictionary).get("code", "")) == "file.missing":
			names.append(String((note as Dictionary).get("name", "")))
	assert_true(names.has("Tmap.trn"), str(names))
	assert_true(names.has("synth_full.env"), str(names))
	assert_eq(int(state.get("body", {}).get("missing", 0)), names.size())
	assert_not_null(_device_node(state, "MissionEnvironment"))
	assert_eq(state.get("items", []).size(), 14, "the marks are the document's, terrain or not")


## S14 (D3): the mission kind keeps two devices. Three missions opened in turn, each ready on its
## device: the third gives up the first's (the least recently used mission's), the second's stands.
func test_two_missions_keep_two_devices() -> void:
	if _app == null:
		return
	assert_true(_open_mission(false, PackedStringArray(["a.bms", "b.bms"])))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_not_null(_app.get_viewport_device("missions/synth_logic.bms", "mission"))
	assert_true(_seam.open_document("missions/a.bms"))
	state = await _await_ready("missions/a.bms")
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_not_null(_app.get_viewport_device("missions/a.bms", "mission"))
	assert_not_null(_app.get_viewport_device("missions/synth_logic.bms", "mission"), "two missions, two devices")
	assert_true(_seam.open_document("missions/b.bms"))
	state = await _await_ready("missions/b.bms")
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_not_null(_app.get_viewport_device("missions/b.bms", "mission"))
	assert_not_null(_app.get_viewport_device("missions/a.bms", "mission"), "the second mission's device stands")
	assert_null(_app.get_viewport_device("missions/synth_logic.bms", "mission"), "the first's, least recently used, given up")
