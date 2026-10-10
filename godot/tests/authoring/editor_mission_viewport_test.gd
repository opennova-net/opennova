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
## project lacks is a note, the picture standing without it (a terrain gone missing taking the ground
## drawn before with it); the mission kind keeps two devices,
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
## its heights, its two maps and the textures it names, its two foliage definitions' models), the
## environment (synth_full.env and its clouds), the item table the mission's items are in, and the models
## their graphics name. `effects` (DI-31): the pumps author a particle slot at their model's ground point,
## its effect in a particle file of the project. `listen` (DI-36): the mission's four waypoint markers (item
## 106005, path 1's stops in master's synth_logic.bms) made the game's env-sound emitters of V_TRUCK_ILP at every
## hour, the fixture bank that holds it (heard to 2 km, its member tone.wav) as the project's game.lwf.
func _mint_project(root: String, skip: PackedStringArray = PackedStringArray(), effects := false,
		listen := false) -> void:
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		if skip.has(name):
			continue
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"), _tga(2048 if name == "mnml_c" or name == "mnml_d1" else 32))
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	# The item table, its pump drawn by the synth crate (one box, no live PANM, no occlusion records):
	# a model the placer batches, so the mission's items are retained statics beside its individual
	# models (the armory's buildings, the persons).
	var items := FileAccess.get_file_as_string(ProjectSettings.globalize_path(FIXTURES + "def/items.def"))
	# The fixture ends its lines in CR LF, the one break the retail def walk splits at.
	assert_true(items.contains("graphic pump\r\n"))
	var crate_line := "graphic crate\r\n" + ("particlefx Puff ground\r\n" if effects else "")
	items = items.replace("graphic pump\r\n", crate_line)
	if listen:
		# The waypoint item the mission's four markers place (the fixture's table has none of it).
		assert_false(items.contains("  id 106005\r\n"))
		var waypoint := "begin \"Waypoint\"\r\n  id 106005\r\n  type marker\r\n  move_function envs\r\n"
		for slot in 4:
			waypoint += "  soundloop_%d V_TRUCK_ILP\r\n" % (slot + 1)
		items += waypoint + "end\r\n"
		_copy_fixture("lwf/menu.lwf", root.path_join("sounds").path_join("game.lwf"))
		_copy_fixture("lwf/tone.wav", root.path_join("sounds").path_join("tone.wav"))
	_write(root.path_join("defs").path_join("items.def"), items.to_utf8_buffer())
	for name in ["crate.3di", "armory.3di", "shed.3di"]:
		_copy_fixture("threedi/synth/" + name, root.path_join("models").path_join(name))
	# The terrain's foliage definitions grow bush1 and bush2 (the synth crate under both names, as the foliage
	# integration test stages them).
	for name in ["bush1.3di", "bush2.3di"]:
		_copy_fixture("threedi/synth/crate.3di", root.path_join("models").path_join(name))
	if effects:
		_write(root.path_join("particles").path_join("fx.ptl"), _ptl().to_utf8_buffer())
		_copy_fixture("cbin/particle_dot.tga", root.path_join("particles").path_join("particle_dot.tga"))


## A particle file of one effect, Puff: a burst of dots of the fixture's graphic emitting for a minute
## (DI-14's recipe).
func _ptl() -> String:
	var text := "[effectdef]\n{\n\tid = Puff;\n\tpdefs = PuffDot;\n}\n\n"
	text += "[particledef]\n{\n\tid = PuffDot;\n\temit_dur = 60;\n\temit_rate = 40;\n\temit_burst = 1;\n"
	text += "\tage = 1.0;\n\tscale = 1.0;\n\tspeed = 1.5;\n\tspread = 40;\n\tgraphic1 = particle_dot.tga, blend;\n"
	text += "\tg1_alpha = 1;\n\tg1_scale = 1;\n}\n\n"
	return text


## A new project holding the minted mission as missions/synth_logic.bms (and, `whole`, the files its
## picture reads), scanned, the mission open.
func _open_mission(whole := true, also: PackedStringArray = PackedStringArray(),
		skip: PackedStringArray = PackedStringArray(), effects := false, listen := false) -> bool:
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
		_mint_project(root, skip, effects, listen)
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


## S14 V8, V9: the mission builds over the Shell's frames. Opened at a budget of 0, its viewport is
## `loading` at the pump that takes it, one unit a frame, its progress never going back, the units'
## labels in the build's order (the environment, the terrain's files, the terrain a tile a step, the
## sky, the water, the item table, a unit per graphic, the placement's units, the static shadows
## bound, the pose), then `ready`
## with the layers under its device, the terrain built (the body's `ground`) and no file missing.
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
	assert_eq(labels, PackedStringArray(["environment", "terrain files", "terrain", "sky", "water", "items", "models",
			"place", "shadows", "pose"]))
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


## DI-29: the ground overlay the options ask reaches the device's terrain as an Update: the surface classes'
## picture (its size the body's), then the foliage's, then none; the build generation stands.
func test_the_ground_overlay_tints_the_terrain() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var terrain: Terrain = _device_node(state, "Terrain")
	assert_not_null(terrain)
	if terrain == null:
		return
	assert_false(terrain.has_ground_overlay(), "no overlay asked")
	assert_true(_change({"kind": "mission", "options": {"overlay": "surfaces"}}))
	_app.pump()
	state = _state()
	var overlay: Dictionary = state.get("body", {}).get("overlay", {})
	assert_eq(String(overlay.get("kind", "")), "surfaces", str(overlay))
	assert_true(terrain.has_ground_overlay(), "the terrain tinted")
	var picture: Texture2D = terrain.get_terrain_material().get_shader_parameter("u_overlay")
	assert_not_null(picture)
	if picture:
		assert_eq(Vector2i(picture.get_width(), picture.get_height()),
				Vector2i(int(overlay.get("width", 0)), int(overlay.get("height", 0))), "the viewport's picture")
	var rect: Vector4 = terrain.get_terrain_material().get_shader_parameter("u_overlay_rect")
	assert_almost_eq(rect.x, float(overlay.get("west", 0.0)), 0.001, "its west edge on the world's x")
	assert_almost_eq(rect.y, -float(overlay.get("north", 0.0)), 0.001, "its north edge on the world's z (the negated y)")
	assert_true(_change({"kind": "mission", "options": {"overlay": "foliage"}}))
	_app.pump()
	state = _state()
	overlay = state.get("body", {}).get("overlay", {})
	assert_eq(String(overlay.get("kind", "")), "foliage", str(overlay))
	assert_true(terrain.has_ground_overlay())
	assert_true(_change({"kind": "mission", "options": {"overlay": "none"}}))
	_app.pump()
	state = _state()
	assert_eq(state.get("body", {}).get("overlay"), null)
	assert_false(terrain.has_ground_overlay(), "asked away, gone")
	assert_eq(int(state.get("builds", 0)), 1, "an overlay is an Update, never a build")


## S14 V8, V10: the ground is the terrain's. A move of the first item with `stick` (the default) keeps
## its height over TerrainData.get_height_world_bilinear at the new place; the build generation
## stands; a vertical ray through the moved item meets the terrain where the height says; an item
## dropped at the picture's middle lands on the terrain with its model's ground point baked in.
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
	# S14 V10: a drop lands on the terrain. The item the crate draws, let go at the picture's middle, is
	# one item more, stored at the ground point less the crate's `ground` user point (an author-time
	# bake): its stored position plus that anchor lies on the terrain. (The synth crate's point is its
	# origin; editor_mission_viewport's ctest mints one off it for the bake's arithmetic.)
	state = _state()
	var ids := {}
	for row: Variant in state.get("items", []):
		ids[int((row as Dictionary).get("id", 0))] = true
	var size: Dictionary = state.get("device", {})
	var dropped: Dictionary = _ask({"kind": "edit_in_viewport", "drop": {"reference": "item", "name": "106100",
			"at": [float(size.get("width", 1024)) * 0.5, float(size.get("height", 768)) * 0.5], "kind": "mission"}})
	assert_true(bool(dropped.get("outcome", {}).get("done", false)), str(dropped))
	var placed := {}
	for row: Variant in _state().get("items", []):
		if not ids.has(int((row as Dictionary).get("id", 0))):
			placed = row
	assert_eq(String(placed.get("kind", "")), "item", "one item more: %s" % str(placed))
	if placed.is_empty():
		return
	var crate: ObjectData = _app.get_mission_placer("missions/synth_logic.bms").object_data_for("crate")
	assert_not_null(crate)
	if crate == null:
		return
	var anchor := Vector3.ZERO
	for i in crate.get_user_point_count():
		var point: ModelUserPoint = crate.get_user_point_info(i)
		if point != null and point.get_name().to_lower() == "ground":
			anchor = MissionObjectPlacer.godot_to_bms_position(point.get_position())
	var on := _vector(placed.get("at")) + anchor
	assert_almost_eq(on.z, _ground(data, on.x, on.y), 0.02, "the anchor on the terrain")


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


## The demo round's bug 5: the mission's terrain set to a name the project lacks drops the ground the
## device drew before (its Terrain holds nothing built), never the old terrain under the note that the
## new one is missing.
func test_a_terrain_gone_missing_drops_its_ground() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_true(bool(state.get("body", {}).get("ground", false)), "the terrain built")
	var terrain: Terrain = _device_node(state, "Terrain")
	assert_not_null(terrain)
	if terrain == null:
		return
	assert_true(terrain.is_built(), "the terrain drawn")
	var page: Dictionary = _seam.query("document", {"path": MISSION_PATH, "limit": 1})
	var rows: Array = page.get("rows", [])
	assert_false(rows.is_empty(), str(page).left(300))
	if rows.is_empty():
		return
	var header_id := int((rows[0] as Dictionary).get("id", 0))
	var builds := int(state.get("builds", 0))
	var edited: Dictionary = _ask({"kind": "edit_record", "path": MISSION_PATH,
			"edits": [{"op": "set", "id": header_id, "field": "terrain", "value": "nightisle"}]})
	assert_true(bool(edited.get("outcome", {}).get("done", false)), str(edited).left(300))
	# The header's terrain is a Rebuild: ready again once the device has built anew.
	for _frame in 900:
		state = _state()
		if String(state.get("status", "")) == "ready" and int(state.get("builds", 0)) > builds:
			break
		await get_tree().process_frame
	_app.pump()
	state = _state()
	assert_gt(int(state.get("builds", 0)), builds, "built again")
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	assert_false(bool(state.get("body", {}).get("ground", true)), "no terrain: no ground")
	var names: PackedStringArray = []
	for note: Variant in state.get("notes", []):
		if String((note as Dictionary).get("code", "")) == "file.missing":
			names.append(String((note as Dictionary).get("name", "")))
	assert_true(names.has("nightisle.trn"), str(names))
	terrain = _device_node(state, "Terrain")
	assert_not_null(terrain)
	if terrain != null:
		assert_false(terrain.is_built(), "the ground drawn before is gone")
		assert_eq(terrain.get_patches_active(), 0, "no patch of the old terrain drawn")


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


## S14 review m13: a device given up while it builds. Three missions over the whole minted project
## opened in turn at a budget of 0, each a few units into its build when the next opens: the third
## gives up the first's device mid-build (nothing of its build runs on, no error), and the third builds
## to ready.
func test_a_device_given_up_mid_build() -> void:
	if _app == null:
		return
	_app.build_budget_ms = 0
	assert_true(_open_mission(true, PackedStringArray(["a.bms", "b.bms"])))
	for _frame in 12:
		await get_tree().process_frame
	assert_eq(String(_state().get("status", "")), "loading", "the first mid-build")
	assert_true(_seam.open_document("missions/a.bms"))
	for _frame in 12:
		await get_tree().process_frame
	assert_true(_seam.open_document("missions/b.bms"))
	_app.pump()
	assert_null(_app.get_viewport_device("missions/synth_logic.bms", "mission"), "the first's device given up mid-build")
	var state := await _await_ready("missions/b.bms")
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	assert_true(bool(state.get("body", {}).get("ground", false)), "the third built whole")


const MISSION_PATH := "missions/synth_logic.bms"


## The mission device's read-back through EditorApp's typed seams: its placer, its counts, each
## entity's key by row (empty: no mission device).
func _mission_device() -> Dictionary:
	if int(_app.get_mission_device_count(MISSION_PATH, "placements")) < 0:
		return {}
	var out := {"placer": _app.get_mission_placer(MISSION_PATH)}
	for what in ["placements", "placed", "lifted", "hidden", "place_us"]:
		out[what] = int(_app.get_mission_device_count(MISSION_PATH, what))
	var keys := {}
	for row: Variant in _state().get("items", []):
		var id := int((row as Dictionary).get("id", 0))
		keys[id] = int(_app.get_mission_entity_key(MISSION_PATH, id))
	out["keys"] = keys
	return out


## The placer's individual models by the key each carries.
func _models_by_key(placer: MissionObjectPlacer) -> Dictionary:
	var out := {}
	for model: Variant in placer.get_placed_models():
		var ref: EntityRef = (model as ObjectModel).get_entity_ref()
		if ref != null:
			out[ref.get_bms_id()] = model
	return out


## The transform the placement draws the entity of mark `mark` at (its position, its yaw, its item's
## scale; the fixture's pitch and roll are 0).
func _placed_transform(placer: MissionObjectPlacer, mark: Dictionary) -> Transform3D:
	return placer.item_entity_transform(_vector(mark.get("at")), Vector3(0.0, float(mark.get("yaw", 0)), 0.0),
			int(mark.get("item", 0)))


## The first entity mark of `kind` whose entity the placement drew `drawn` ("static": a retained
## static, "model": an individual model); empty for none.
func _placed_mark(state: Dictionary, device: Dictionary, drawn: String, kind := "") -> Dictionary:
	var placer: MissionObjectPlacer = device.get("placer")
	var models := _models_by_key(placer)
	var keys: Dictionary = device.get("keys", {})
	for row: Variant in state.get("items", []):
		var mark: Dictionary = row
		var mark_kind := String(mark.get("kind", ""))
		if mark_kind == "area" or mark_kind == "marker" or (not kind.is_empty() and mark_kind != kind):
			continue
		var key := int(keys.get(int(mark.get("id", 0)), 0))
		if key == 0:
			continue
		if drawn == "model" and models.has(key):
			return mark
		if drawn == "static" and not models.has(key) and placer.get_static_instance_lod(key) > -2:
			return mark
	return {}


## S14 V9: the entities are the game's placer's. Ready, the device holds one whole placement, every
## entity placed (none lifted or hidden) under its MissionObjects container; each one that draws is
## a retained static or an individual model, at the transform the placement draws its item at
## (MissionObjectPlacer.item_entity_transform), and the fixture has both.
func test_the_entities_are_placed() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device := _mission_device()
	assert_false(device.is_empty(), "the mission device")
	var placer: MissionObjectPlacer = device.get("placer")
	assert_not_null(placer, "its placer, the item table read")
	if placer == null:
		return
	assert_eq(int(device.get("placements", 0)), 1)
	assert_eq(int(device.get("placed", 0)), 12, "every entity placed")
	assert_eq(int(device.get("lifted", -1)), 0)
	assert_eq(int(device.get("hidden", -1)), 0)
	assert_not_null(_device(state).find_child("MissionObjects", true, false), "the placer's container")
	var models := _models_by_key(placer)
	var keys: Dictionary = device.get("keys", {})
	var statics := 0
	var individuals := 0
	for row: Variant in state.get("items", []):
		var mark: Dictionary = row
		var kind := String(mark.get("kind", ""))
		if kind == "area" or kind == "marker":
			continue
		var key := int(keys.get(int(mark.get("id", 0)), 0))
		assert_gt(key, 0, "placed: a key of the placement's (%s)" % str(mark))
		if models.has(key):
			individuals += 1
			assert_true((models[key] as ObjectModel).transform.is_equal_approx(_placed_transform(placer, mark)),
					"the individual model where the placement draws it: %s" % str(mark))
		else:
			statics += 1
			assert_gt(placer.get_static_instance_lod(key), -2, "a retained static: %s" % str(mark))
			var at: Variant = placer.get_static_instance_transform(key)
			assert_true(at is Transform3D and (at as Transform3D).is_equal_approx(_placed_transform(placer, mark)),
					"the static where the placement draws it: %s" % str(mark))
	assert_gt(statics, 0, "the fixture's statics")
	assert_gt(individuals, 0, "the fixture's individual models (its persons)")


## S14 V9: a move updates in place and never places again. A wire drag of a retained static and of an
## individual model each moves it where the placement would draw it now (the static's rows rewritten,
## its read-back the new transform; the model's node), the build generation and the placement count
## standing. A building's terrain shadow source follows at the gesture's end, not each sample: with
## the gesture open its revision stands, the last sample ends it and the revision moves.
func test_a_move_updates_and_never_places_again() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device := _mission_device()
	var placer: MissionObjectPlacer = device.get("placer")
	assert_not_null(placer)
	if placer == null:
		return
	var keys: Dictionary = device.get("keys", {})
	var builds := int(state.get("builds", 0))
	var still := _placed_mark(state, device, "static")
	var person := _placed_mark(state, device, "model")
	assert_false(still.is_empty(), "a retained static")
	assert_false(person.is_empty(), "an individual model")
	if still.is_empty() or person.is_empty():
		return
	var key := int(keys.get(int(still["id"]), 0))
	var moved: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": int(still["id"]), "handle": "move", "by": [40, 0], "kind": "mission"}})
	assert_true(bool(moved.get("outcome", {}).get("done", false)), str(moved))
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a move is an Update")
	var now := _mark_of(state, int(still["id"]))
	assert_false(_vector(now.get("at")).is_equal_approx(_vector(still.get("at"))), "the static moved")
	var at: Variant = placer.get_static_instance_transform(key)
	assert_true(at is Transform3D and (at as Transform3D).is_equal_approx(_placed_transform(placer, now)),
			"its rows where the placement would draw it now")
	assert_false(placer.get_static_instance_live_populations(key).is_empty(), "its rows stand in their populations")
	var model: ObjectModel = _models_by_key(placer)[int(keys.get(int(person["id"]), 0))]
	moved = _ask({"kind": "edit_in_viewport", "drag": {"id": int(person["id"]), "handle": "move", "by": [0, 30],
			"kind": "mission"}})
	assert_true(bool(moved.get("outcome", {}).get("done", false)), str(moved))
	state = _state()
	assert_true(model.transform.is_equal_approx(_placed_transform(placer, _mark_of(state, int(person["id"])))),
			"the individual model's node where the placement would draw it now")
	assert_eq(int(state.get("builds", 0)), builds)
	assert_eq(int(_mission_device().get("placements", 0)), 1, "placed once")
	# A building's terrain shadow source: at the gesture's end.
	var building := {}
	for row: Variant in state.get("items", []):
		if String((row as Dictionary).get("kind", "")) == "building":
			building = row
			break
	assert_false(building.is_empty())
	if building.is_empty():
		return
	var revision := placer.get_static_terrain_shadow_source_revision()
	var first: Dictionary = _ask({"kind": "edit_in_viewport", "drag": {"id": int(building["id"]), "handle": "move",
			"by": [20, 0], "end": false, "kind": "mission"}})
	var gesture := int(first.get("outcome", {}).get("gesture", 0))
	assert_gt(gesture, 0, str(first))
	_app.pump()
	assert_eq(placer.get_static_terrain_shadow_source_revision(), revision, "a gesture open: the shadow source waits")
	var last: Dictionary = _ask({"kind": "edit_in_viewport", "drag": {"id": int(building["id"]), "handle": "move",
			"by": [20, 0], "gesture": gesture, "end": true, "kind": "mission"}})
	assert_true(bool(last.get("outcome", {}).get("done", false)), str(last))
	_app.pump()
	assert_ne(placer.get_static_terrain_shadow_source_revision(), revision, "the gesture ended: the source follows")


## S14 V9: an add lifts, a remove hides. A duplicate of an item (a graphic already warm) is made whole
## as it is taken (ready at the pump that takes it, one build more, nothing placed again): one model
## lifted under Lifted. Removed, the lifted model is hidden; a retained static removed is hidden (its
## rows out of every population), and the removal undone shows it again.
func test_an_add_lifts_and_a_remove_hides() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var builds := int(state.get("builds", 0))
	var ids := {}
	for row: Variant in state.get("items", []):
		ids[int((row as Dictionary).get("id", 0))] = true
	var item := _first_item(state)
	assert_true(_seam.duplicate_record(int(item.get("id", 0))))
	_app.pump()
	state = _state()
	assert_eq(String(state.get("status", "")), "ready", "made whole as it is taken: %s" % str(state.get("progress")))
	assert_eq(int(state.get("builds", 0)), builds + 1)
	var device := _mission_device()
	assert_eq(int(device.get("lifted", 0)), 1, str(device))
	assert_eq(int(device.get("placements", 0)), 1, "nothing placed again")
	var lifted: Node = _device(state).find_child("Lifted", true, false)
	assert_not_null(lifted)
	if lifted == null:
		return
	assert_eq(lifted.get_child_count(), 1, "one model lifted")
	var copy := 0
	for row: Variant in state.get("items", []):
		var id := int((row as Dictionary).get("id", 0))
		if not ids.has(id) and String((row as Dictionary).get("kind", "")) == "item":
			copy = id
	assert_gt(copy, 0, "the copy's mark")
	assert_true(_seam.remove_record(copy))
	_app.pump()
	device = _mission_device()
	assert_eq(int(device.get("lifted", -1)), 0)
	assert_eq(int(device.get("hidden", 0)), 1, "the lifted model hidden")
	assert_false((lifted.get_child(0) as Node3D).visible)
	var placer: MissionObjectPlacer = device.get("placer")
	var still := _placed_mark(_state(), device, "static")
	assert_false(still.is_empty())
	if still.is_empty():
		return
	var key := int((device.get("keys", {}) as Dictionary).get(int(still["id"]), 0))
	assert_true(_seam.remove_record(int(still["id"])))
	_app.pump()
	assert_true(placer.is_static_instance_hidden(key), "the static hidden")
	assert_true(placer.get_static_instance_live_populations(key).is_empty(), "its rows out of every population")
	assert_eq(int(_mission_device().get("hidden", 0)), 2)
	_seam.undo()
	_app.pump()
	assert_false(placer.is_static_instance_hidden(key), "the removal undone: shown again")
	assert_eq(int(_mission_device().get("placements", 0)), 1, "still the one placement")


## S14 V9: a file the entities read that moves places them again, the layers that did not read it
## kept. The items' model written again (a new stamp) and the project rescanned: the device mounts its
## files afresh and places every entity again (a second whole placement over a new placer), its
## terrain data the same as before.
func test_a_file_change_places_again() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var terrain: Terrain = _device_node(state, "Terrain")
	var data: TerrainData = terrain.get_terrain_data()
	assert_not_null(data)
	var placer: MissionObjectPlacer = _mission_device().get("placer")
	var epoch := ResourceRoot.cache_epoch()
	var root: String = _seam.get_project_root()
	var crate := root.path_join("models").path_join("crate.3di")
	var bytes := FileAccess.get_file_as_bytes(crate)
	# A new stamp: the size moves with a byte past the end the reader never reads.
	_write(crate, bytes + PackedByteArray([0]))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps")
	state = await _await_ready()
	for _frame in 600:
		if int(_mission_device().get("placements", 0)) >= 2 and String(state.get("status", "")) == "ready":
			break
		await get_tree().process_frame
		state = _state()
	var device := _mission_device()
	assert_eq(int(device.get("placements", 0)), 2, "a second whole placement")
	assert_ne(device.get("placer"), placer, "the entities placed again over the files mounted afresh")
	assert_eq(terrain.get_terrain_data(), data, "the terrain, which read nothing that moved, kept")
	assert_eq(String(state.get("status", "")), "ready", str(state))
	# The device's mount dropped its own root's caches, never the process's (review m4: another
	# device's placer keeps its own).
	assert_eq(ResourceRoot.cache_epoch(), epoch, "the global cache epoch stands")


## The crate's model written again (a new stamp: a byte past the end the reader never reads) and the
## project rescanned, settled (the pumps take the Rebuild; no frame has stepped a unit yet).
func _touch_the_crate() -> void:
	var crate: String = _seam.get_project_root().path_join("models").path_join("crate.3di")
	_write(crate, FileAccess.get_file_as_bytes(crate) + PackedByteArray([0]))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps")


## The frames stepped (a budget of 0: a unit a frame) until `done` says so or `frames` ran out; the
## labels seen, in order, each once per run of it.
func _step_until(done: Callable, frames := 900) -> PackedStringArray:
	var labels := PackedStringArray()
	var state := _state()
	for _frame in frames:
		var label := String(state.get("progress", {}).get("label", "")) if state.get("progress") is Dictionary else ""
		if not label.is_empty() and (labels.is_empty() or labels[labels.size() - 1] != label):
			labels.append(label)
		if done.call(state):
			break
		await get_tree().process_frame
		state = _state()
	return labels


## S14 review M1: no device writes a process-wide shader global outside its publication. Headless no
## canvas draws the picture, so nothing presents or publishes: the mission builds to ready, takes a time
## of day (an Update), moves an entity, and builds its entities again for a moved file, and neither
## the environment nor the water writes a global meanwhile (MissionEnvironment.get_global_writes,
## Water.get_global_writes: a headless renderer keeps no global to read back). Both hold their
## globals; the time reaches the environment's clock (6.5 h is minute 390) and a layer switched off
## hides its node (review m13).
func test_no_global_written_outside_a_publication() -> void:
	if _app == null:
		return
	_app.build_budget_ms = 0
	var environment_writes := MissionEnvironment.get_global_writes()
	var water_writes := Water.get_global_writes()
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var environment: MissionEnvironment = _device_node(state, "MissionEnvironment")
	var water: Water = _device_node(state, "Water")
	assert_not_null(environment)
	assert_not_null(water)
	if environment == null or water == null:
		return
	assert_true(environment.is_globals_held() and water.is_globals_held(), "the device's environment and water hold")
	assert_true(_change({"kind": "mission", "options": {"time": 6.5}}))
	assert_almost_eq(environment.get_mission_minute_of_day(), 390.0, 0.5, "the time reached the clock")
	var item := _first_item(_state())
	var moved: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": int(item.get("id", 0)), "handle": "move", "by": [30, 0], "kind": "mission"}})
	assert_true(bool(moved.get("outcome", {}).get("done", false)), str(moved))
	_touch_the_crate()
	state = await _await_ready()
	for _frame in 600:
		if int(_mission_device().get("placements", 0)) >= 2 and String(state.get("status", "")) == "ready":
			break
		await get_tree().process_frame
		state = _state()
	assert_eq(int(_mission_device().get("placements", 0)), 2, "the entities built again")
	assert_eq(MissionEnvironment.get_global_writes(), environment_writes, "the environment wrote no global")
	assert_eq(Water.get_global_writes(), water_writes, "the water wrote no global")
	# A layer off: its node hidden.
	assert_true(_change({"kind": "mission", "options": {"show": {"terrain": false, "sky": false}}}))
	assert_false((_device_node(state, "Terrain") as Node3D).visible, "the terrain off")
	assert_false((_device_node(state, "SkyDome") as Node3D).visible, "the sky off")


## S14 review M5: what a Rebuild replaces leaves in a unit, never as it is taken. A moved model file
## (the entities' layer read it): at the pumps that take the Rebuild the last whole scene stands (the
## placer's container in the tree, nothing retired or queued to go), and the build's first unit drops
## it; the build ends with the entities placed again.
func test_a_rebuild_keeps_the_last_scene_until_its_units_run() -> void:
	if _app == null:
		return
	_app.build_budget_ms = 0
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device := _device(state)
	var container: Node = device.find_child("MissionObjects", true, false)
	assert_not_null(container)
	if container == null:
		return
	var children := container.get_child_count()
	assert_gt(children, 0)
	_touch_the_crate()
	state = _state()
	assert_eq(String(state.get("status", "")), "loading", str(state).left(300))
	assert_eq(String(state.get("progress", {}).get("label", "")), "drop", "the drop is the build's first unit")
	assert_true(is_instance_valid(container) and container.is_inside_tree() and not container.is_queued_for_deletion(),
			"the last whole scene stands as the Rebuild is taken")
	assert_eq(container.get_child_count(), children)
	assert_true(device.find_children("Retired", "", true, false).is_empty(), "nothing retired yet")
	state = await _await_ready()
	for _frame in 600:
		if int(_mission_device().get("placements", 0)) >= 2 and String(state.get("status", "")) == "ready":
			break
		await get_tree().process_frame
		state = _state()
	assert_eq(int(_mission_device().get("placements", 0)), 2)
	assert_eq(int(_mission_device().get("placed", 0)), 12, "every entity placed again")


## S14 review M9: a whole placement cut short by a Rebuild is placed whole again. The model file moved
## (a whole placement), a duplicate made while its place units run: the build that follows places
## every entity (the copy among them) and lifts none, never leaving the half the cut placement made.
func test_a_placement_cut_short_places_whole() -> void:
	if _app == null:
		return
	_app.build_budget_ms = 0
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var item := _first_item(state)
	_touch_the_crate()
	# Into the placement: its second unit run.
	var seen := 0
	for _frame in 900:
		state = _state()
		if String(state.get("progress", {}).get("label", "") if state.get("progress") is Dictionary else "") == "place":
			seen += 1
			if seen >= 2:
				break
		await get_tree().process_frame
	assert_eq(seen, 2, "the placement under way")
	assert_true(_seam.duplicate_record(int(item.get("id", 0))))
	_app.pump()
	state = await _await_ready()
	for _frame in 900:
		if int(_mission_device().get("placements", 0)) >= 2 and String(state.get("status", "")) == "ready":
			break
		await get_tree().process_frame
		state = _state()
	var device := _mission_device()
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	assert_eq(int(device.get("placed", 0)), 13, "every entity placed, the copy among them: %s" % str(device))
	assert_eq(int(device.get("lifted", -1)), 0, "none lifted over a half placement")
	assert_eq(int(device.get("hidden", -1)), 0)


## S14 review m6: a Rebuild while the first build makes the terrain carries that work on. Opened at a
## budget of 0, a duplicate made while the terrain's tiles build: the build that follows goes on with
## the terrain's units (no terrain file read again) and ends ready with the ground.
func test_a_rebuild_while_the_terrain_builds_carries_it_on() -> void:
	if _app == null:
		return
	_app.build_budget_ms = 0
	assert_true(_open_mission())
	_app.pump()
	var at_terrain := func(state: Dictionary) -> bool:
		var progress: Variant = state.get("progress")
		return progress is Dictionary and String((progress as Dictionary).get("label", "")) == "terrain" and \
				int((progress as Dictionary).get("done", 0)) > 20
	var before: PackedStringArray = await _step_until(at_terrain)
	assert_true(before.has("terrain"), str(before))
	var item := _first_item(_state())
	assert_true(_seam.duplicate_record(int(item.get("id", 0))))
	_app.pump()
	var ready := func(state: Dictionary) -> bool: return String(state.get("status", "")) == "ready"
	var after: PackedStringArray = await _step_until(ready)
	assert_false(after.has("terrain files"), "no terrain file read again: %s" % str(after))
	assert_false(after.has("environment"), "the environment stands")
	assert_eq(after[0] if not after.is_empty() else "", "terrain", "the terrain's units went on first: %s" % str(after))
	var state := _state()
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	assert_true(bool(state.get("body", {}).get("ground", false)), "the terrain built")
	assert_eq(int(_mission_device().get("placed", 0)), 13, "the copy placed with the rest")


## S14 review m5: a terrain without its height data is a note, never a failed picture. The minted
## project without Tmap.cpt: the viewport is ready, its picture with no terrain (no ground), the .cpt
## among its notes, the entities placed all the same.
func test_a_terrain_without_its_height_data_is_a_note() -> void:
	if _app == null:
		return
	assert_true(_open_mission(true, PackedStringArray(), PackedStringArray(["Tmap.cpt"])))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	assert_false(bool(state.get("body", {}).get("ground", true)), "no terrain built: no ground")
	var names: PackedStringArray = []
	for note: Variant in state.get("notes", []):
		if String((note as Dictionary).get("code", "")) == "file.missing":
			names.append(String((note as Dictionary).get("name", "")).to_lower())
	assert_true(names.has("tmap.cpt"), str(names))
	assert_eq(int(_mission_device().get("placed", 0)), 12, "the entities placed")


## A placed tile at x_fixed (16.16) of tile index.
func _tile(x_fixed: int, index: int) -> TerrainTileEntry:
	var entry := TerrainTileEntry.new()
	entry.x_fixed = x_fixed
	entry.z_fixed = 0
	entry.set_tile_index(index)
	return entry


## S14 review M4: the device reads the mission's .til as the game does before the terrain builds
## (Terrain.set_tile_info_override), and follows it: a .til written beside the mission reaches the
## terrain (its one tile), one written again with two tiles builds the terrain again with both.
func test_the_mission_til_reaches_the_terrain() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor mission til %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Mission Til"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join("missions").path_join("synth_logic.bms"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION)))
	_mint_project(root)
	var til := TerrainTileInfo.new()
	til.add_entry(_tile(0, 3))
	var til_path := root.path_join("missions").path_join("synth_logic.til")
	assert_eq(til.save_to_path(til_path), OK)
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle())
	assert_true(_seam.open_document("missions/synth_logic.bms"))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	var terrain: Terrain = _device_node(state, "Terrain")
	assert_not_null(terrain)
	if terrain == null:
		return
	var read: TerrainTileInfo = terrain.get_tile_info_override()
	assert_not_null(read, "the mission's .til on the terrain")
	if read != null:
		assert_eq(read.get_entry_count(), 1)
	til.add_entry(_tile(65536 * 8, 4))
	assert_eq(til.save_to_path(til_path), OK)
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle())
	state = await _await_ready()
	for _frame in 600:
		var now: TerrainTileInfo = terrain.get_tile_info_override()
		if now != null and now.get_entry_count() == 2 and String(_state().get("status", "")) == "ready":
			break
		await get_tree().process_frame
	read = terrain.get_tile_info_override()
	assert_true(read != null and read.get_entry_count() == 2, "the .til followed: the terrain built again with both")


## S14 review M3, M7: an entity's attributes place it as the game does. The first retained static
## item given the Reflective attribute (0x00800000) is lifted, built as the placement builds an
## entity's model: reflected in the water mirror (as its attributes say), its EntityRef keyed past
## every placed key; given its attributes back (an undo), the placed static shows again and nothing
## stays lifted.
func test_attributes_lift_and_undo_shows_the_placed_one() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device := _mission_device()
	var placer: MissionObjectPlacer = device.get("placer")
	var still := _placed_mark(state, device, "static", "item")
	assert_false(still.is_empty(), "a retained static item")
	if still.is_empty() or placer == null:
		return
	var key := int((device.get("keys", {}) as Dictionary).get(int(still["id"]), 0))
	assert_true(_seam.set_field(int(still["id"]), "ai_flags", 0x00800000))
	_app.pump()
	state = _state()
	device = _mission_device()
	assert_eq(int(device.get("lifted", 0)), 1, "another attribute: lifted: %s" % str(device))
	assert_true(placer.is_static_instance_hidden(key), "the placed static hidden meanwhile")
	var lifted: Node = _device(state).find_child("Lifted", true, false)
	var model: ObjectModel = lifted.get_child(lifted.get_child_count() - 1) if lifted != null and lifted.get_child_count() > 0 else null
	assert_not_null(model, "its model under Lifted")
	if model != null:
		assert_true(model.mirror_reflected, "Reflective: drawn in the water mirror, as the placement draws it")
		var ref: EntityRef = model.get_entity_ref()
		assert_not_null(ref, "its EntityRef, as the placement's models carry")
		if ref != null:
			assert_gt(ref.get_bms_id(), 12, "keyed past every placed key")
	_seam.undo()
	_app.pump()
	device = _mission_device()
	assert_eq(int(device.get("lifted", -1)), 0, "given back what was placed: nothing lifted")
	assert_false(placer.is_static_instance_hidden(key), "the placed static shows again")
	assert_eq(int(device.get("placements", 0)), 1, "nothing placed again")


## The picture point of the nearest of `faces` (three vertices a triangle, through `xform`) to the camera
## whose middle is on the picture `size` and past the pick slop from mark `mark`'s glyph (twice it):
## where a press on the entity's surface goes. Vector2(-1, -1): none.
func _face_point(camera: Camera3D, faces: PackedVector3Array, xform: Transform3D, mark: Dictionary, size: Vector2) -> Vector2:
	var glyph := Vector2(float(mark["screen"][0]), float(mark["screen"][1]))
	var best := Vector2(-1, -1)
	var nearest := INF
	var i := 0
	while i + 2 < faces.size():
		var middle := (xform * faces[i] + xform * faces[i + 1] + xform * faces[i + 2]) / 3.0
		i += 3
		if camera.is_position_behind(middle):
			continue
		var at := camera.unproject_position(middle)
		if at.x < 2.0 or at.y < 2.0 or at.x > size.x - 2.0 or at.y > size.y - 2.0 or at.distance_to(glyph) < 16.0:
			continue
		var away := camera.global_position.distance_to(middle)
		if away < nearest:
			nearest = away
			best = at
	return best


## Every face an individual model's meshes draw, in the picture's space (three vertices a triangle).
func _model_faces(model: ObjectModel) -> PackedVector3Array:
	var out := PackedVector3Array()
	for node: Node in model.find_children("*", "MeshInstance3D", true, false):
		var mesh := node as MeshInstance3D
		if mesh.mesh == null:
			continue
		for vertex in mesh.mesh.get_faces():
			out.append(mesh.global_transform * vertex)
	return out


## The polish after its review (H1, M2): a press takes what the pointer is over, as the game's own choice
## among the entities a ray's broad phase passes is the face it hits. The device answers what the
## camera's ray meets first (the placed entities' faces, the nearer first, before the terrain): framed
## close, a retained static hit through its nearest face away from its glyph is its record, and so is an
## individual model; the ground just beside the static, past its faces yet within the sphere about its
## anchor that holds them (what a sphere alone would take), is no record, nor is the sky.
func test_a_hit_takes_the_face_the_ray_meets() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device := _mission_device()
	var placer: MissionObjectPlacer = device.get("placer")
	assert_not_null(placer)
	if placer == null:
		return
	var keys: Dictionary = device.get("keys", {})
	var wire: Dictionary = state.get("device", {})
	var size := Vector2(float(wire.get("width", 1024)), float(wire.get("height", 768)))
	var still := _placed_mark(state, device, "static")
	var person := _placed_mark(state, device, "model")
	assert_false(still.is_empty(), "a retained static")
	assert_false(person.is_empty(), "an individual model")
	if still.is_empty() or person.is_empty():
		return
	# Both set down on the terrain (the minted mission's entities stand under it), so nothing of the
	# ground stands between the camera and their faces.
	var grounded: Dictionary = _ask({"kind": "edit_in_viewport", "command": {"name": "ground",
			"ids": [int(still["id"]), int(person["id"])], "kind": "mission"}})
	assert_true(bool(grounded.get("outcome", {}).get("done", false)), str(grounded))
	state = _state()
	still = _mark_of(state, int(still["id"]))
	person = _mark_of(state, int(person["id"]))
	# The static framed close: its graphic's faces where its rows draw it.
	var at: Variant = placer.get_static_instance_transform(int(keys.get(int(still["id"]), 0)))
	assert_true(at is Transform3D, "the static's rows")
	var faces := placer.get_static_graphic_faces(placer.graphic_for(int(still.get("item", 0))))
	assert_false(faces.is_empty(), "the static's graphic warm, its faces kept")
	if not at is Transform3D or faces.is_empty():
		return
	var xform: Transform3D = at
	assert_true(_change({"kind": "mission", "camera": {"target": still["at"], "yaw": 30, "pitch": 35, "distance": 6}}))
	state = _state()
	var camera := _camera(state)
	assert_not_null(camera)
	if camera == null:
		return
	still = _mark_of(state, int(still["id"]))
	var point := _face_point(camera, faces, xform, still, size)
	assert_true(point.x >= 0.0, "a face of the static on the picture, off its glyph")
	var hit := _viewport("hit", {"x": point.x, "y": point.y})
	assert_eq(int(hit.get("id", 0)), int(still["id"]), "the static's face: its record: %s" % str(hit))
	# The ground beside it, toward the camera, past its faces' reach about its anchor yet within the sphere
	# there that holds them: no record.
	var terrain: Terrain = _device_node(state, "Terrain")
	var data: TerrainData = terrain.get_terrain_data() if terrain != null else null
	assert_not_null(data)
	if data != null:
		var origin := xform.origin
		var across := 0.0
		var reach := 0.0
		for vertex in faces:
			var world := xform * vertex
			across = maxf(across, Vector2(world.x - origin.x, world.z - origin.z).length())
			reach = maxf(reach, world.distance_to(origin))
		var toward := Vector3(camera.global_position.x - origin.x, 0.0, camera.global_position.z - origin.z).normalized()
		var beside := origin + toward * lerpf(across, reach, 0.25)
		beside.y = data.get_height_world_bilinear(beside)
		assert_gt(reach, across, "the sphere about the anchor holding the faces reaches past them on the ground")
		assert_lt(beside.distance_to(origin), reach, "the point within that sphere")
		var ground := camera.unproject_position(beside)
		var glyph := Vector2(float(still["screen"][0]), float(still["screen"][1]))
		assert_gt(ground.distance_to(glyph), 8.0, "off the glyph")
		hit = _viewport("hit", {"x": ground.x, "y": ground.y})
		assert_eq(int(hit.get("id", 0)), 0, "the ground beside the static: no record: %s" % str(hit))
	# The sky, the camera level three metres over the static: no record.
	var over := _vector(still["at"]) + Vector3(0.0, 0.0, 3.0)
	assert_true(_change({"kind": "mission", "camera": {"target": [over.x, over.y, over.z], "pitch": 0, "distance": 6}}))
	hit = _viewport("hit", {"x": size.x * 0.5, "y": 4.0})
	assert_eq(int(hit.get("id", 0)), 0, "the sky: no record: %s" % str(hit))
	# The individual model framed close: its meshes' faces where its node stands.
	var model: ObjectModel = _models_by_key(placer).get(int(keys.get(int(person["id"]), 0)))
	assert_not_null(model)
	if model == null:
		return
	assert_true(_change({"kind": "mission", "camera": {"target": person["at"], "yaw": 200, "pitch": 20, "distance": 5}}))
	state = _state()
	camera = _camera(state)
	person = _mark_of(state, int(person["id"]))
	point = _face_point(camera, _model_faces(model), Transform3D(), person, size)
	assert_true(point.x >= 0.0, "a face of the model on the picture, off its glyph")
	hit = _viewport("hit", {"x": point.x, "y": point.y})
	assert_eq(int(hit.get("id", 0)), int(person["id"]), "the model's face: its record: %s" % str(hit))


## The minted project's rifleman (106102, the mission's two organics: one on path 1, one with none) made a
## person the game poses: org1 with aidata, drawn by the synth person rig (19 bones), its soldier.adm and
## clips beside it (fixtures/anim). Rescanned, the device built again over it.
func _people_project() -> void:
	var root: String = _seam.get_project_root()
	var items_path := root.path_join("defs").path_join("items.def")
	var items := FileAccess.get_file_as_string(items_path)
	var rifleman := "  id 106102\r\n  type person\r\n  graphic shed\r\n  anim_def soldier\r\n"
	assert_true(items.contains(rifleman), "the fixture's rifleman")
	items = items.replace(rifleman,
			"  id 106102\r\n  type person\r\n  graphic person\r\n  anim_def soldier\r\n  ai_function org1\r\n  attrib: aidata\r\n")
	_write(items_path, items.to_utf8_buffer())
	_copy_fixture("threedi/synth/person.3di", root.path_join("models").path_join("person.3di"))
	for name in ["soldier.adm", "idle.bad", "walk.bad"]:
		_copy_fixture("anim/" + name, root.path_join("anim").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps")


## The device's model of the organic mark `mark` (null: none).
func _person_model(mark: Dictionary) -> ObjectModel:
	var device := _mission_device()
	var placer: MissionObjectPlacer = device.get("placer")
	if placer == null:
		return null
	return _models_by_key(placer).get(int(device.get("keys", {}).get(int(mark.get("id", 0)), 0)))


## The model holds the pose `pose` (the viewport's `pose` of its mark): the clip it names at its playhead, as
## the game's presenter poses a body row (ObjectModel.play_body_clip_at, SkeletalAnim's tick clock).
func _assert_posed(model: ObjectModel, pose: Dictionary, what: String) -> void:
	var playing: Dictionary = pose.get("playing", {})
	assert_eq(model.get_active_body_clip(), String(playing.get("row", "")), what + ": its clip")
	assert_eq(model.get_active_body_variant(), int(playing.get("variant", -1)), what + ": its ring entry")
	assert_false(model.has_body_blend(), what + ": the reset blended out")
	var skeletal: SkeletalAnim = model.get_skeletal_anim()
	assert_not_null(skeletal, what + ": the rig bound")
	if skeletal != null:
		var seconds := skeletal.get_clip_phase_seconds(String(playing.get("row", "")), int(playing.get("phase", 0)),
				int(playing.get("variant", 0)), -1)
		assert_almost_eq(model.get_animation_time(), seconds, 1e-4, what + ": its playhead")


## DI-38: the people stand as the game spawns them. Each organic's mark carries the pose the game's organic
## init and warmup leave it in (the one on path 1 walking, the other idle, at the playheads their SSNs'
## warmups leave), and the device poses each person's model in it; a route taken away is an Update that
## poses the person idle, the build generation standing.
func test_people_posed_as_they_spawn() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	var before := int(state.get("builds", 0))
	_people_project()
	# The item table and the models moved: built again over them.
	for _frame in 900:
		state = _state()
		if String(state.get("status", "")) == "ready" and int(state.get("builds", 0)) > before:
			break
		await get_tree().process_frame
	_app.pump()
	state = _state()
	assert_gt(int(state.get("builds", 0)), before, "built again")
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	# The people play their clips on the clock (S23 C): held at its start, they stand at their spawn.
	assert_true(_change({"kind": "mission", "clock": {"playing": false, "ticks": 0}}))
	for _frame in 3:
		await get_tree().process_frame
		_app.pump()
	state = _state()
	assert_eq(int(state.get("body", {}).get("posed", -1)), 2, "both people posed")
	var walking := {}
	var standing := {}
	for row: Variant in state.get("items", []):
		var mark: Dictionary = row
		if String(mark.get("kind", "")) != "organic":
			continue
		var pose: Dictionary = mark.get("pose", {})
		assert_eq(String(pose.get("status", "")), "posed", str(pose))
		if String(pose.get("because", "")) == "route":
			walking = mark
		else:
			standing = mark
	assert_false(walking.is_empty(), "the organic on path 1 walks")
	assert_false(standing.is_empty(), "the other stands")
	if walking.is_empty() or standing.is_empty():
		return
	assert_eq(String(walking["pose"].get("row", "")), "anim_walk_forward")
	assert_eq(String(standing["pose"].get("row", "")), "anim_idle")
	assert_eq(String(walking["pose"]["playing"].get("clip", "")), "walk.bad")
	var placer: MissionObjectPlacer = _mission_device().get("placer")
	for mark: Dictionary in [walking, standing]:
		var model := _person_model(mark)
		assert_not_null(model, "the person's model: %s" % str(mark).left(200))
		if model != null:
			_assert_posed(model, mark["pose"], String(mark.get("name", "")))
			# It stands where its spawn stands it: the record raised by the warmup, then stood on the terrain by
			# the ground solve (the minted terrain is read: settled).
			var lift := float(mark["pose"].get("lift", 0.0))
			assert_gt(lift, 0.0, "the spawn lifts the person")
			assert_true(bool(mark["pose"].get("settled", false)), "stood on the terrain")
			assert_true(model.transform.is_equal_approx(
					_placed_transform(placer, mark).translated(Vector3(0.0, lift, 0.0))), "lifted by its lift")
	# Its route taken away: an Update, the person posed idle on the model the placement made.
	var builds := int(state.get("builds", 0))
	var model := _person_model(walking)
	var edited: Dictionary = _ask({"kind": "edit_record", "path": MISSION_PATH,
			"edits": [{"op": "set", "id": int(walking["id"]), "field": "waypoint_id", "value": 0}]})
	assert_true(bool(edited.get("outcome", {}).get("done", false)), str(edited).left(300))
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a route's edit is an Update")
	var now := _mark_of(state, int(walking["id"]))
	assert_eq(String(now.get("pose", {}).get("row", "")), "anim_idle")
	assert_eq(_person_model(walking), model, "the same model")
	if model != null:
		_assert_posed(model, now["pose"], "idle now")


## S23 C: the people play their clips on the preview clock from their spawn, the device posing each person's model by
## its body as it plays now (its pose's `now` on the wire); held, they hold; a seek back to the start stands them at
## their spawn again.
func test_people_play_their_clips() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	var before := int(state.get("builds", 0))
	_people_project()
	for _frame in 900:
		state = _state()
		if String(state.get("status", "")) == "ready" and int(state.get("builds", 0)) > before:
			break
		await get_tree().process_frame
	assert_true(_change({"kind": "mission", "clock": {"playing": true, "rate": 1}}))
	for _frame in 30:
		await get_tree().process_frame
		_app.pump()
	assert_true(_change({"kind": "mission", "clock": {"playing": false}}))
	for _frame in 3:
		await get_tree().process_frame
		_app.pump()
	state = _state()
	var people: Dictionary = state.get("body", {}).get("people", {})
	assert_eq(int(people.get("playing", -1)), 2, str(people))
	assert_gt(int(people.get("tick", 0)), int(people.get("started_at", 0)), "played on: %s" % str(people))
	var played := 0
	for row: Variant in state.get("items", []):
		var mark: Dictionary = row
		if String(mark.get("kind", "")) != "organic":
			continue
		var now: Dictionary = mark.get("pose", {}).get("now", {})
		assert_false(now.is_empty(), str(mark.get("pose", {})).left(300))
		var model := _person_model(mark)
		assert_not_null(model)
		if model == null or now.is_empty():
			continue
		_assert_posed(model, now, String(mark.get("name", "")) + " now")
		if int(now["playing"].get("phase", -1)) != int(mark["pose"]["playing"].get("phase", -1)):
			played += 1
	assert_eq(played, 2, "both played on from their spawn")
	# A seek back to the start: at their spawn again.
	assert_true(_change({"kind": "mission", "clock": {"ticks": 0}}))
	for _frame in 3:
		await get_tree().process_frame
		_app.pump()
	state = _state()
	for row: Variant in state.get("items", []):
		var mark: Dictionary = row
		if String(mark.get("kind", "")) != "organic":
			continue
		var model := _person_model(mark)
		if model != null:
			_assert_posed(model, mark["pose"], String(mark.get("name", "")) + " at its spawn")


## DI-31: the device draws single-sampled (the particle renderer's passes bind its depth, DI-14's rule) and
## carries the game's foliage beside the terrain and its particle renderer; the three layers are options on
## the wire and the body says what the device drew of each.
func test_the_device_draws_the_games_foliage_effects_and_lights() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_eq(int(state.get("body", {}).get("missing", -1)), 0, str(state.get("notes", [])))
	var device := _device(state)
	assert_not_null(device)
	if device == null:
		return
	assert_eq(device.msaa_3d, Viewport.MSAA_DISABLED, "single-sampled, as the game's view draws")
	assert_not_null(_device_node(state, "FoliageDispatcher"), "the game's foliage beside the terrain")
	assert_not_null(_device_node(state, "ParticleRenderer"), "the game's particle renderer")
	var show: Dictionary = state.get("options", {}).get("show", {})
	assert_true(bool(show.get("foliage", false)) and bool(show.get("effects", false)) and bool(show.get("lights", false)),
			"the three layers on by default: %s" % str(show))
	for _frame in 4:
		_app.pump()
		await get_tree().process_frame
	var drawn: Dictionary = _state().get("body", {}).get("drawn", {})
	assert_true(drawn.has("foliage") and drawn.has("lights") and drawn.has("effects"), str(drawn))
	assert_eq(int(drawn.get("foliage", {}).get("slots", 0)), 2, "Tmap's two definitions configured: %s" % str(drawn))


## DI-31: each placed item's particle slot as the mission's start attaches it: the pumps author Puff at their
## model's ground point, the viewport plays the scene and the device's particle renderer draws it; the layer off
## closes the scene.
func test_items_attach_their_effects() -> void:
	if _app == null:
		return
	assert_true(_open_mission(true, PackedStringArray(), PackedStringArray(), true))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var effects: Dictionary = state.get("body", {}).get("effects", {})
	assert_gt(int(effects.get("attached", 0)), 0, "the pumps' slots attached: %s" % str(effects))
	assert_eq(int(effects.get("emitters", 0)), int(effects.get("attached", 0)), "once each, at its ground point")
	var renderer: ParticleRenderer = _device_node(state, "ParticleRenderer")
	assert_not_null(renderer)
	if renderer == null:
		return
	for _frame in 30:
		_app.pump()
		await get_tree().process_frame
	state = _state()
	effects = state.get("body", {}).get("effects", {})
	assert_gt(int(effects.get("particles", 0)), 0, "the scene holds live particles: %s" % str(effects))
	var named: Array = effects.get("effects", [])
	assert_eq(named.size(), 1)
	if not named.is_empty():
		assert_eq(String((named[0] as Dictionary).get("defined_in", "")), "particles/fx.ptl")
	var carried := 0
	for row: Variant in state.get("items", []):
		var effect: Dictionary = (row as Dictionary).get("effect", {})
		if String(effect.get("status", "")) == "attached":
			carried += 1
			assert_eq(effect.get("points", []), ["ground"], "at the crate's ground point")
	assert_eq(carried, int(effects.get("attached", 0)), "each pump's mark names its slot")
	assert_true(_change({"kind": "mission", "options": {"show": {"effects": false}}}))
	_app.pump()
	assert_eq(_state().get("body", {}).get("effects"), null, "the layer off: no scene")


## DI-31: the lights the game lights the scene with: the armory's two LGHT records spawned for each placed
## armory as the mission's start spawns them (the device's state, drawn or not; the frame's selection is the
## windowed test's); the layer off empties the pool.
func test_the_placed_models_light_the_scene() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var lights := {}
	for _frame in 30:
		_app.pump()
		await get_tree().process_frame
		lights = _state().get("body", {}).get("drawn", {}).get("lights", {})
		if int(lights.get("pool", 0)) > 0:
			break
	assert_gte(int(lights.get("pool", 0)), 4, "two lights an armory, two armories: %s" % str(lights))
	var builds := int(_state().get("builds", 0))
	assert_true(_change({"kind": "mission", "options": {"show": {"lights": false}}}))
	for _frame in 4:
		_app.pump()
		await get_tree().process_frame
	lights = _state().get("body", {}).get("drawn", {}).get("lights", {})
	assert_eq(int(lights.get("pool", -1)), 0, "the layer off: no light: %s" % str(lights))
	assert_eq(int(_state().get("builds", 0)), builds, "an Update, never a build")


## DI-23: the Shoot tool. A click's `shoot` at the picture's middle (where the device's ray meets the ground) fires
## the picked ammo there, the impact as the game plays it in the body's shots (the row for the class the game reads
## there), its effect spawned in the items' effect scene (DI-31's, the device's one particle renderer drawing it);
## a shot at an item's place from above stops on its faces and scars it, the device's ScarPresenter drawing the
## scar; Clear shots drops them, their effects with them.
func test_the_shoot_tool_plays_its_impacts() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	var root: String = _seam.get_project_root()
	var rows := ""
	for tag in ["obj", "dirt", "grass", "snow", "cement", "sand", "packeddirt", "water", "railroad", "mud", "ice",
			"quicksand", "stone", "wood", "metal", "glass", "cloth", "foliage", "hmetal", "flesh"]:
		rows += "\t\t%s Puff S_HIT 15\n" % tag
	_write(root.path_join("defs/ammo.def"), TestFs.crlf("ammo AT_NULL\nend\nammo AMMO_T\n\tvelocity 800\n\tmax_age 2\n"
			+ "\tweight_in_grains 62\n\tscar_type 1\n\teffects_table\n" + rows + "\tend\nend\n").to_utf8_buffer())
	var ptl := "[effectdef]\n{\n\tid = Puff;\n\tpdefs = PuffDot;\n}\n\n[particledef]\n{\n\tid = PuffDot;\n\temit_dur = 30;\n"
	ptl += "\temit_rate = 40;\n\temit_burst = 1;\n\tage = 1.0;\n\tscale = 1.0;\n\tspeed = 1.5;\n\tspread = 40;\n"
	ptl += "\tgraphic1 = particle_dot.tga, blend;\n\tg1_alpha = 1;\n\tg1_scale = 1;\n}\n\n"
	_write(root.path_join("particles/puff.ptl"), ptl.to_utf8_buffer())
	_copy_fixture("cbin/particle_dot.tga", root.path_join("particles/particle_dot.tga"))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps")
	assert_true(_change({"kind": "mission", "options": {"tool": "shoot", "ammo": "AMMO_T"}}))
	state = _state()
	assert_eq(String(state.get("options", {}).get("tool", "")), "shoot")
	assert_true(String(state.get("body", {}).get("hint", "")).begins_with("Shoot AMMO_T"), str(state.get("body", {}).get("hint")))
	var device := _device(state)
	assert_not_null(device)
	if device == null:
		return
	# A click's shot at the picture's middle: the ground there.
	var size: Dictionary = state.get("device", {})
	var shot: Dictionary = _ask({"kind": "edit_in_viewport", "command": {"name": "shoot", "kind": "mission",
			"at": [float(size.get("width", 1024)) * 0.5, float(size.get("height", 768)) * 0.5]}})
	assert_true(bool(shot.get("outcome", {}).get("done", false)), str(shot).left(300))
	var impact := {}
	for _frame in 300:
		_app.pump()
		await get_tree().process_frame
		for event: Variant in _state().get("body", {}).get("shots", {}).get("events", []):
			if String((event as Dictionary).get("kind", "")) == "impact":
				impact = event
		if not impact.is_empty():
			break
	assert_false(impact.is_empty(), "the shot stops: %s" % str(_state().get("body", {}).get("shots", {})).left(400))
	if impact.is_empty():
		return
	assert_eq(int(impact.get("tag", -1)), int(impact.get("surface", -9)) + 4, "the row for the class struck")
	assert_eq(String(impact.get("effect", "")), "Puff")
	assert_eq(device.msaa_3d, Viewport.MSAA_DISABLED, "single-sampled, as the game's view draws (DI-31)")
	var renderers := device.find_children("*", "ParticleRenderer", true, false)
	assert_eq(renderers.size(), 1, "one particle renderer: the shots' effects are the items' scene's")
	var spawned: Array = _state().get("body", {}).get("shots", {}).get("effects", [])
	assert_false(spawned.is_empty(), "the impact's effect in the scene")
	if not spawned.is_empty():
		assert_eq(String((spawned[0] as Dictionary).get("effect", "")), "Puff")
		assert_eq(String((spawned[0] as Dictionary).get("defined_in", "")), "particles/puff.ptl")
	# A shot at an item from above: its faces stop the round, a scar drawn on it.
	var item := _first_item(state)
	assert_false(item.is_empty())
	if item.is_empty():
		return
	# The fixture's item stands buried: set down on the ground first (a scar is written above the water alone).
	assert_true(bool(_ask({"kind": "edit_in_viewport", "command": {"name": "ground", "kind": "mission",
			"ids": [int(item["id"])]}}).get("outcome", {}).get("done", false)))
	item = _mark_of(_state(), int(item["id"]))
	var at := _vector(item.get("at"))
	assert_true(_change({"kind": "mission", "shot": {"ammo": "AMMO_T", "at": [at.x, at.y, at.z],
			"eye": [at.x, at.y, at.z + 60.0]}, "clock": {"playing": true}}))
	var scars := device.find_children("ShotScars", "ScarPresenter", true, false)
	assert_false(scars.is_empty(), "the shots' ScarPresenter")
	if scars.is_empty():
		return
	var presenter := scars[0] as ScarPresenter
	var shots := {}
	for _frame in 300:
		_app.pump()
		await get_tree().process_frame
		shots = _state().get("body", {}).get("shots", {})
		if int(shots.get("scars", 0)) > 0 and presenter.get_stats_record().world_surfaces > 0:
			break
	assert_gt(int(shots.get("scars", 0)), 0, "the item scarred: %s" % str(shots.get("events", [])).left(600))
	assert_gt(presenter.get_stats_record().world_surfaces, 0, "the game's ScarPresenter draws it")
	# Clear shots: nothing of them stands.
	assert_true(bool(_ask({"kind": "edit_in_viewport", "command": {"name": "clear_shots", "kind": "mission"}})
			.get("outcome", {}).get("done", false)))
	for _frame in 5:
		_app.pump()
		await get_tree().process_frame
	assert_eq(_state().get("body", {}).get("shots", {}).get("shots", [1]).size(), 0)
	assert_eq(_state().get("body", {}).get("shots", {}).get("effects", [1]).size(), 0, "their effects gone")
	# A shot after the clear runs again over the mission (what it fires in configured afresh).
	assert_true(_change({"kind": "mission", "shot": {"ammo": "AMMO_T", "at": [at.x, at.y, at.z],
			"eye": [at.x, at.y, at.z + 60.0]}, "clock": {"playing": true}}))
	var again := false
	for _frame in 300:
		_app.pump()
		await get_tree().process_frame
		for event: Variant in _state().get("body", {}).get("shots", {}).get("events", []):
			again = again or String((event as Dictionary).get("kind", "")) == "impact"
		if again:
			break
	assert_true(again, "a shot after the clear stops: %s" % str(_state().get("body", {}).get("shots", {})).left(400))


## S23 C: a shot that destroys an item swaps its husk in, as the game's destruction presenter does. The pump made a
## one-hit-point gnrc with the shed as its husk: shot from above, it dies, and its death's swap tick on (the gnrc's
## four-tick think) its retained static's rows are hidden and the husk grafted where it stands; Clear shots lets the
## husk go and shows the pump again.
func test_a_shot_swaps_in_the_husk() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_ready()
	var root: String = _seam.get_project_root()
	var items_path := root.path_join("defs").path_join("items.def")
	var items := FileAccess.get_file_as_string(items_path)
	const PUMP := "  id 106100\r\n  type object\r\n  graphic crate\r\n"
	assert_true(items.contains(PUMP), "the fixture's pump")
	items = items.replace(PUMP, PUMP + "  husk shed\r\n  ai_function gnrc\r\n  hp 1\r\n")
	_write(items_path, items.to_utf8_buffer())
	_write(root.path_join("defs/ammo.def"), TestFs.crlf("ammo AT_NULL\nend\nammo AMMO_T\n\tvelocity 800\n\tmax_age 2\n"
			+ "\tweight_in_grains 62\n\tscar_type 1\nend\n").to_utf8_buffer())
	var before := int(state.get("builds", 0))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps")
	for _frame in 900:
		state = _state()
		if String(state.get("status", "")) == "ready" and int(state.get("builds", 0)) > before:
			break
		await get_tree().process_frame
	assert_eq(String(state.get("status", "")), "ready", str(state).left(300))
	var pump := {}
	for row: Variant in state.get("items", []):
		if int((row as Dictionary).get("item", 0)) == 106100:
			pump = row
			break
	assert_false(pump.is_empty(), "a pump in the mission")
	if pump.is_empty():
		return
	assert_true(bool(_ask({"kind": "edit_in_viewport", "command": {"name": "ground", "kind": "mission",
			"ids": [int(pump["id"])]}}).get("outcome", {}).get("done", false)))
	pump = _mark_of(_state(), int(pump["id"]))
	var at := _vector(pump.get("at"))
	var placer: MissionObjectPlacer = _app.get_mission_placer(MISSION_PATH)
	var key := int(_app.get_mission_entity_key(MISSION_PATH, int(pump["id"])))
	assert_gt(key, 0, "the pump placed")
	assert_false(placer.is_static_instance_hidden(key), "a retained static, shown")
	assert_true(_change({"kind": "mission", "shot": {"ammo": "AMMO_T", "at": [at.x, at.y, at.z],
			"eye": [at.x, at.y, at.z + 60.0]}, "clock": {"playing": true}}))
	var husk: ObjectModel = null
	var deaths: Array = []
	for _frame in 300:
		_app.pump()
		await get_tree().process_frame
		deaths = _state().get("body", {}).get("shots", {}).get("deaths", [])
		var found := _device(_state()).find_children("HuskModel_%d" % int(pump["id"]), "ObjectModel", true, false)
		if not found.is_empty():
			husk = found[0]
			break
	assert_eq(deaths.size(), 1, "the pump destroyed: %s" % str(_state().get("body", {}).get("shots", {}).get("events", [])).left(500))
	if not deaths.is_empty():
		assert_eq(String((deaths[0] as Dictionary).get("husk", "")), "shed")
		assert_true(bool((deaths[0] as Dictionary).get("swaps", false)))
	assert_not_null(husk, "the husk swapped in")
	if husk == null:
		return
	assert_eq(String(husk.get_graphic_name()).to_lower(), "shed")
	assert_true(placer.is_static_instance_hidden(key), "the pump's rows hidden under the husk")
	var placed := MissionObjectPlacer.bms_to_godot_position(at)
	assert_almost_eq(husk.transform.origin, placed, Vector3(0.01, 0.01, 0.01), "the husk where the pump stands")
	# Clear shots: the husk goes, the pump shows again.
	assert_true(bool(_ask({"kind": "edit_in_viewport", "command": {"name": "clear_shots", "kind": "mission"}})
			.get("outcome", {}).get("done", false)))
	for _frame in 5:
		_app.pump()
		await get_tree().process_frame
	assert_false(placer.is_static_instance_hidden(key), "the pump shown again")
	assert_true(_device(_state()).find_children("HuskModel_%d" % int(pump["id"]), "ObjectModel", true, false).is_empty()
			or not is_instance_valid(husk) or husk.is_queued_for_deletion(), "the husk let go")


## DI-36: the Listen. The fixture's four waypoint markers made the game's env-sound emitters of a set the
## project's bank holds: listening near one, the viewport's mix binds each marker's layer to one of the game's
## channels, and the device plays each looping at its marker (an AudioStreamPlayer3D under its world, no
## attenuation: the mix's volume; the device's camera the listener), held paused while its picture is not drawn
## (a headless run draws none); off, the device lets its voices go.
func test_the_listen_plays_the_mixs_channels() -> void:
	if _app == null:
		return
	assert_true(_open_mission(true, PackedStringArray(), PackedStringArray(), false, true))
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_eq(state.get("body", {}).get("listen"), null, "off by default")
	var device := _device(state)
	assert_not_null(device)
	if device == null:
		return
	assert_true(device.is_audio_listener_3d(), "the device's camera is its world's listener")
	assert_true(_change({"kind": "mission", "camera": {"target": [-190, -200, 0], "pitch": 60, "distance": 10},
			"options": {"time": 12, "listen": {"on": true, "volume": 0.5}}}))
	var listen := {}
	for _frame in 60:
		_app.pump()
		await get_tree().process_frame
		var body: Variant = _state().get("body", {}).get("listen")
		listen = body if body is Dictionary else {}
		if listen.get("channels", []).size() >= 4:
			break
	var channels: Array = listen.get("channels", [])
	assert_eq(channels.size(), 4, "a channel a marker: %s" % str(listen))
	for channel: Variant in channels:
		assert_eq(String((channel as Dictionary).get("set", "")), "V_TRUCK_ILP")
		assert_eq(String((channel as Dictionary).get("source", "")), "marker")
		assert_eq(String((channel as Dictionary).get("path", "")), "sounds/tone.wav")
	assert_eq(int(listen.get("sources", {}).get("playing", 0)), 4, str(listen.get("sources", {})))
	# The device plays them: a looping, unattenuated voice a channel, at the channel's place.
	var voices: Array[Node] = []
	for _frame in 120:
		_app.pump()
		await get_tree().process_frame
		voices = device.find_children("ListenChannel*", "AudioStreamPlayer3D", true, false)
		if voices.size() >= 4:
			break
	assert_eq(voices.size(), 4, "a voice a channel")
	# What the device reports reaches the viewport at the next pump.
	_app.pump()
	var drawn: Dictionary = _state().get("body", {}).get("drawn", {}).get("listen", {})
	assert_eq(int(drawn.get("voices", 0)), 4, str(drawn))
	# Sounding while its picture is drawn, held while not (a headless run draws none).
	var audible := bool(drawn.get("audible", false))
	assert_eq(int(drawn.get("playing", -1)), 4 if audible else 0, str(drawn))
	var at := MissionObjectPlacer.bms_to_godot_position(Vector3(-200, -200, 0))
	for voice: Node in voices:
		var player := voice as AudioStreamPlayer3D
		assert_eq(player.attenuation_model, AudioStreamPlayer3D.ATTENUATION_DISABLED)
		assert_eq((player.stream as AudioStreamWAV).loop_mode, AudioStreamWAV.LOOP_FORWARD)
		assert_lt(player.volume_db, 0.0, "the mix's volume, the master's half")
	var near := 0
	for voice: Node in voices:
		near += 1 if (voice as AudioStreamPlayer3D).position.distance_to(at) < 0.01 else 0
	assert_eq(near, 1, "one voice at the marker the camera looks at")
	# Off: the voices go.
	assert_true(_change({"kind": "mission", "options": {"listen": {"on": false}}}))
	for _frame in 4:
		_app.pump()
		await get_tree().process_frame
	assert_eq(_state().get("body", {}).get("listen"), null, "off: no listen")
	assert_eq(device.find_children("ListenChannel", "AudioStreamPlayer3D", true, false).size(), 0, "off: no voice")


## S23 C: the mission's 2D map (kind "map"), the Preview window's beside the 3D view, the state of it.
func _map_state() -> Dictionary:
	return _viewport("state", {"limit": 200, "kind": "map"})


func _await_map() -> Dictionary:
	var state := _map_state()
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		await get_tree().process_frame
		state = _map_state()
	_app.pump()
	return _map_state()


## S23 C: the map's device draws the game's commander map pass over the project's files: the runtime's HudOverlay
## holds its state (the terrain read, the project's HUD layout), the pass's terrain triangles drawn into the
## SubViewport's canvas; a pin per entity and area, each where the CMAP's projection puts it (its scale the CMAP's
## law, zoom x 65536 over the picture's width x 200 metres a pixel, north up about the picture's middle).
func test_the_map_draws_the_commander_map() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var state := await _await_map()
	assert_eq(String(state.get("kind", "")), "map", str(state))
	assert_eq(String(state.get("status", "")), "ready", str(state))
	assert_eq(state.get("items", []).size(), 14, "a pin per entity and area")
	var body: Dictionary = state.get("body", {})
	assert_true(bool(body.get("surface", false)), "the terrain read: %s" % str(body))
	var drawn: Dictionary = body.get("drawn", {})
	assert_true(bool(drawn.get("visible", false)), str(drawn))
	assert_gt(int(drawn.get("terrain_tris", 0)), 0, "the commander map's terrain drawn")
	var device := _device(state)
	assert_not_null(device, "the map's device")
	if device == null:
		return
	var overlay := device.find_child("MapState", true, false) as HudOverlay
	assert_not_null(overlay, "the HUD overlay holding the map's state")
	assert_not_null(device.find_child("Map", true, false) as Control, "the canvas the pass draws under")
	var width := float(state.get("device", {}).get("width", 0))
	var height := float(state.get("device", {}).get("height", 0))
	assert_gt(width, 1.0)
	var camera: Dictionary = state.get("camera", {})
	var zoom := float(camera.get("zoom", 0))
	var scale := zoom * 65536.0 / (width * 200.0)
	assert_almost_eq(float(body.get("scale", 0)), scale, scale * 1e-3, "the CMAP's scale")
	var center: Array = camera.get("center", [0, 0])
	for row: Variant in state.get("items", []):
		var pin: Dictionary = row
		var at: Array = pin.get("at", [0, 0])
		var screen: Array = pin.get("screen", [0, 0])
		var x := width * 0.5 + (float(at[0]) - float(center[0])) / scale
		var y := height * 0.5 - (float(at[1]) - float(center[1])) / scale
		assert_almost_eq(float(screen[0]), x, 1.0, "pin %s x" % pin.get("name"))
		assert_almost_eq(float(screen[1]), y, 1.0, "pin %s y" % pin.get("name"))
	# The zoom on the wire: the CMAP's ZOOMIN step, the pass drawn again at it.
	var tris := int(drawn.get("terrain_tris", 0))
	assert_true(bool(_ask({"kind": "edit_in_viewport", "command": {"name": "zoom_in", "kind": "map"}})
			.get("outcome", {}).get("done", false)))
	_app.pump()
	state = _map_state()
	assert_lt(float(state.get("camera", {}).get("zoom", 0)), zoom, "zoomed in: fewer metres across")
	assert_gt(int(state.get("body", {}).get("drawn", {}).get("terrain_tris", 0)), 0, "drawn again (%d before)" % tris)


## S23 C: the map and the 3D view are one document's: a pin hit on the map, selected, is the selection both ring; a
## drag of a pin over the wire is the 3D view's move to the point under the pointer (stick: its height over the map
## device's ground kept), one undo step, the 3D view's mark moved with it.
func test_the_map_and_the_3d_view_share_a_selection_and_a_drag() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var mission := await _await_ready()
	assert_eq(String(mission.get("status", "")), "ready", str(mission))
	var state := await _await_map()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var terrain: Terrain = _device_node(mission, "Terrain")
	assert_not_null(terrain)
	if terrain == null:
		return
	var data: TerrainData = terrain.get_terrain_data()
	var pin := _first_item(state)
	assert_false(pin.is_empty())
	if pin.is_empty():
		return
	var id := int(pin["id"])
	var hit := _viewport("hit", {"x": float(pin["screen"][0]), "y": float(pin["screen"][1]), "kind": "map"})
	assert_eq(int(hit.get("id", 0)), id, str(hit))
	assert_true(_seam.select_record(id))
	_app.pump()
	assert_true(bool(_mark_of(_map_state(), id).get("selected", false)), "the map rings it")
	assert_true(bool(_mark_of(_state(), id).get("selected", false)), "the 3D view rings it")
	assert_eq(_map_state().get("body", {}).get("selected", []), [id])
	# The drag: 30 pixels east, 20 north.
	var before := _vector(_mark_of(_state(), id).get("at"))
	var clearance := before.z - _ground(data, before.x, before.y)
	var scale := float(state.get("body", {}).get("scale", 1))
	var moved: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": id, "handle": "move", "by": [30, -20], "kind": "map"}})
	assert_true(bool(moved.get("outcome", {}).get("done", false)), str(moved))
	var after := _vector(_mark_of(_state(), id).get("at"))
	assert_almost_eq(after.x, before.x + 30.0 * scale, 0.01, "east by the pixels")
	assert_almost_eq(after.y, before.y + 20.0 * scale, 0.01, "north by the pixels")
	assert_almost_eq(after.z, _ground(data, after.x, after.y) + clearance, 0.002, "stick: its height over the ground")
	var on_map: Dictionary = _mark_of(_map_state(), id)
	assert_almost_eq(float(on_map.get("at", [0, 0])[0]), after.x, 0.001, "the map's pin moved with it")
	_seam.undo()
	_app.pump()
	assert_true(_vector(_mark_of(_state(), id).get("at")).is_equal_approx(before), "undone in one step")
	# A handle the map has not: refused, naming the 3D view.
	var refused: Dictionary = _ask({"kind": "edit_in_viewport",
			"drag": {"id": id, "handle": "yaw", "by": [30, 0], "kind": "map"}})
	assert_false(bool(refused.get("outcome", {}).get("done", true)), str(refused))


## S23 C (the maintainer's ask): the map draws each placed model as its wireframe seen from above, on its device (the
## Outlines node's lines, as many edges as the viewport draws), each footprint where the 3D view's device places the
## model: the vertices of its LOD 0 mesh carried by the placement's transform, seen from above, span the footprint.
func test_the_map_draws_the_models_from_above() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	var mission := await _await_ready()
	assert_eq(String(mission.get("status", "")), "ready", str(mission))
	var state := await _await_map()
	for _frame in 120:
		if int(state.get("body", {}).get("outlines", {}).get("pending", 1)) == 0:
			break
		await get_tree().process_frame
		_app.pump()
		state = _map_state()
	_app.pump()
	state = _map_state()
	var outlines: Dictionary = state.get("body", {}).get("outlines", {})
	assert_eq(int(outlines.get("pending", -1)), 0, str(outlines))
	assert_gt(int(outlines.get("edges", 0)), 0, str(outlines))
	assert_eq(int(state.get("body", {}).get("drawn", {}).get("outline_edges", -1)), int(outlines.get("edges", 0)))
	assert_not_null(_device(state).find_child("Outlines", true, false), "the wireframes' node")
	var placer: MissionObjectPlacer = _mission_device().get("placer")
	assert_not_null(placer)
	if placer == null:
		return
	var compared := 0
	for row: Variant in state.get("items", []):
		var pin: Dictionary = row
		if not pin.has("footprint"):
			continue
		var mark := _mark_of(mission, int(pin["id"]))
		var data: ObjectData = placer.object_data_for(placer.graphic_for(int(mark.get("item", 0))))
		if data == null:
			continue
		var at := _placed_transform(placer, mark)
		var lo := Vector2(INF, INF)
		var hi := Vector2(-INF, -INF)
		for surface: Variant in data.build_lod_submeshes(0):
			var mesh := (surface as Dictionary).get("mesh") as ArrayMesh
			for index in mesh.get_surface_count():
				for vertex: Vector3 in mesh.surface_get_arrays(index)[Mesh.ARRAY_VERTEX]:
					var p := MissionObjectPlacer.godot_to_bms_position(at * vertex)
					lo = lo.min(Vector2(p.x, p.y))
					hi = hi.max(Vector2(p.x, p.y))
		var flo := Vector2(INF, INF)
		var fhi := Vector2(-INF, -INF)
		for corner: Variant in pin["footprint"]:
			flo = flo.min(Vector2(float(corner[0]), float(corner[1])))
			fhi = fhi.max(Vector2(float(corner[0]), float(corner[1])))
		assert_almost_eq(flo.x, lo.x, 0.01, "%s west" % pin.get("name"))
		assert_almost_eq(flo.y, lo.y, 0.01, "%s south" % pin.get("name"))
		assert_almost_eq(fhi.x, hi.x, 0.01, "%s east" % pin.get("name"))
		assert_almost_eq(fhi.y, hi.y, 0.01, "%s north" % pin.get("name"))
		compared += 1
	assert_gt(compared, 1, "the placed models compared")
