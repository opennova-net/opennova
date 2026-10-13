extends GutTest

## The maps and the tiles shown in the mission's 3D view as they are made (ADR 0046 S25, the maps and the tiles: the
## device patches), headless over the editor-enabled GDExtension: a tile stamped through the Tiles tool's request and
## a colour stroke and a foliage stroke are each an Update of the picture (its builds stand, the device's Terrain node
## and its data stay the ones it built), the device's terrain takes them in place (its tile override holds the stamped
## tile, its colour map image the painted texels, its tile cache's pages recomposed with the art, its foliage map the
## painted codes), and the mission's Undo puts each back.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION_FIXTURE := "res://../fixtures/bms/synth_logic.bms"
const FIXTURES := "res://../fixtures/"
const MISSION := "missions/synth_logic.bms"
const TILES := "missions/synth_logic.til"
const COLOUR_MAP := "terrain/mnml_c.tga"
const TERRAIN_TEXTURES := ["mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1"]
# Mission (-200, 200): the Tmap terrain's sector at grid (3, 3), quadrant 1. A tile's corner at (-208, 208) (its
# north-west: the record's x and stored z) covers the 16 units east and south of it, the point among them.
const AT := Vector2(-200.0, 200.0)
const CORNER := Vector2(-208.0, 208.0)
# The tile set: four cells of 64 texels (red, green, blue, white), two across.
const CELL_COLOURS := [Color(1, 0, 0), Color(0, 1, 0), Color(0, 0, 1), Color(1, 1, 1)]

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor maps device %d" % Time.get_ticks_usec())
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


## A 32-bit uncompressed TGA, the top row first, each texel `texel_at(x, y)`.
func _tga(size: Vector2i, texel_at: Callable) -> PackedByteArray:
	var bytes := TestFs.tga_bytes(size)
	for y in size.y:
		for x in size.x:
			var colour: Color = texel_at.call(x, y)
			var at := 18 + (y * size.x + x) * 4
			bytes[at] = colour.b8
			bytes[at + 1] = colour.g8
			bytes[at + 2] = colour.r8
			bytes[at + 3] = 255
	return bytes


## A new project holding the minted mission on the Tmap terrain (its .trn, heights, maps, a grey colour map and a
## tile set of four coloured cells) and its environment, scanned, the mission open.
func _open_mission() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor maps device project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Maps Device"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join(MISSION), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION_FIXTURE)))
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"),
				TestFs.tga_bytes(Vector2i(2048, 2048) if name == "mnml_d1" else Vector2i(32, 32)))
	_write(root.path_join(COLOUR_MAP), TestFs.tga_bytes(Vector2i(1024, 1024), Color8(128, 128, 128)))
	_write(root.path_join("terrain/mnml_t.tga"), _tga(Vector2i(128, 128),
			func(x: int, y: int) -> Color: return CELL_COLOURS[(y / 64) * 2 + x / 64]))
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return _seam.open_document(MISSION)


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "limit": 10, "path": MISSION})


func _await_ready() -> Dictionary:
	var state := _state()
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0 and not bool(state.get("device", {}).get("build", {}).get("loading", true)):
			break
		await get_tree().process_frame
		_app.pump()
		state = _state()
	_app.pump()
	return _state()


func _terrain(state: Dictionary) -> Terrain:
	var device: SubViewport = _app.get_viewport_device(String(state.get("path", "")), String(state.get("kind", "")))
	if device == null:
		return null
	var found := device.find_children("*", "Terrain", true, false)
	return found[0] as Terrain if not found.is_empty() else null


## The camera over the corner, looking down on it: the pages there are the ones drawn.
func _look_at_corner() -> void:
	var answer: Dictionary = _seam.request({"kind": "set_viewport", "path": MISSION, "viewport": {"kind": "mission",
			"camera": {"target": [AT.x, AT.y, 30.0], "distance": 60.0, "pitch": 60.0, "yaw": 0.0}}})
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer.get("outcome")))
	_app.pump()


## The device's frames until its tile cache settles, its diagnostics then. Headless, no canvas presents the device:
## its terrain leg (MissionViewportApplier::present's render_frame) is run here, over the device's own camera.
func _settled(terrain: Terrain) -> Dictionary:
	terrain.set_tile_cache_capture_diagnostics(true)
	var diagnostics := {}
	for _frame in 600:
		_app.pump()
		terrain.render_frame()
		await get_tree().process_frame
		diagnostics = terrain.get_tile_cache_diagnostics()
		if int(diagnostics.get("pending_jobs", -1)) == 0 and int(diagnostics.get("frame_requests", 0)) > 0 \
				and int(diagnostics.get("frame_ready_hits", -1)) == int(diagnostics.get("frame_requests", 0)):
			break
	return diagnostics


## The mission's 2D map (kind "map"), its state once its device drew.
func _map_state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "limit": 10, "path": MISSION, "kind": "map"})


func _map_drawn_tiles() -> int:
	for _frame in 120:
		_app.pump()
		await get_tree().process_frame
	return int(_map_state().get("body", {}).get("drawn", {}).get("tiles", -1))


func _stamp(cell: int) -> Dictionary:
	var answer: Dictionary = _seam.request({"kind": "edit_record", "path": TILES, "terrain_view": MISSION, "edits": [
		{"op": "add", "kind": "tile", "field": "tile", "value": cell, "as": "t"},
		{"op": "set", "id": "t", "field": "x", "value": CORNER.x},
		{"op": "set", "id": "t", "field": "y", "value": CORNER.y},
	]})
	_app.pump()
	return answer


func test_a_stamped_tile_is_an_update_its_pages_draw() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	_look_at_corner()
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var builds := int(state.get("builds", 0))
	var terrain := _terrain(state)
	assert_not_null(terrain, "the device built its terrain")
	if terrain == null:
		return
	var data := terrain.get_terrain_data()
	var before := await _settled(terrain)
	assert_gt(int(before.get("ready_pages", 0)), 0, "the device composed its pages: %s" % str(before))
	assert_eq(await _map_drawn_tiles(), 0, "the 2D map draws no tile: %s" % str(_map_state().get("body")))
	var map_builds := int(_map_state().get("builds", 0))

	# The first tile: the mission's .til made and opened, the tile laid; the device takes it in place.
	var stamped := _stamp(1)
	assert_true(bool(stamped.get("outcome", {}).get("done", false)), str(stamped.get("outcome")))
	var after := await _settled(terrain)
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a stamp is an Update: nothing builds again")
	assert_eq(_terrain(state), terrain, "the device's terrain stands")
	assert_eq(terrain.get_terrain_data(), data, "with its data")
	var tiles := terrain.get_tile_info_override()
	assert_not_null(tiles, "the device's terrain holds the mission's tiles")
	if tiles != null:
		assert_eq(tiles.get_entry_count(), 1, "the stamped tile")
		if tiles.get_entry_count() == 1:
			var entry := tiles.get_entry(0)
			assert_eq(entry.get_tile_index(), 1)
	assert_true(bool(after.get("tile_overlay_available", false)), "the tile set is a page source: %s" % str(after))
	assert_ne(int(after.get("resident_output_hash", 0)), int(before.get("resident_output_hash", 0)),
			"the pages under the tile show its art")
	# The 2D map draws it too, as MED's 2D view does: an Update of its picture.
	assert_eq(await _map_drawn_tiles(), 1, "the 2D map draws the stamped tile")
	assert_eq(int(_map_state().get("builds", 0)), map_builds, "the map's picture stands")

	# Undone: the device's pages as they were.
	assert_true(bool(_seam.request({"kind": "undo", "path": TILES}).get("outcome", {}).get("done", false)))
	var undone := await _settled(terrain)
	assert_eq(int(_state().get("builds", 0)), builds, "the undo is an Update")
	assert_eq(int(undone.get("resident_output_hash", 0)), int(before.get("resident_output_hash", 0)),
			"the pages as the terrain built them")
	assert_eq(await _map_drawn_tiles(), 0, "the 2D map draws none again")


func test_a_colour_stroke_is_an_update_its_pages_draw() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	_look_at_corner()
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var builds := int(state.get("builds", 0))
	var terrain := _terrain(state)
	assert_not_null(terrain, "the device built its terrain")
	if terrain == null:
		return
	var data := terrain.get_terrain_data()
	var before := await _settled(terrain)

	var answer: Dictionary = _seam.request({"kind": "terrain_stroke", "path": MISSION, "stroke": {"plane": "colour",
			"brush": {"tool": "colour", "colour": "#FF2000", "radius": 6, "strength": 1}, "points": [[AT.x, AT.y]],
			"gesture": 0, "end": true}})
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer.get("outcome")))
	var after := await _settled(terrain)
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a stroke is an Update: nothing builds again")
	assert_eq(_terrain(state), terrain, "the device's terrain stands")
	assert_eq(terrain.get_terrain_data(), data, "with its data")
	var image := data.get_colormap_image()
	assert_not_null(image, "the device's data holds the painted colour map")
	if image != null:
		# Around the atlas texel under (-200, 200), (312, 312): the colour map is 1024 texels over the 1024-unit atlas.
		var painted := 0
		for y in range(296, 329):
			for x in range(296, 329):
				var c := image.get_pixel(x, y)
				if c.r8 == 255 and c.g8 == 32 and c.b8 == 0:
					painted += 1
		assert_gt(painted, 10, "the stroke's texels in the device's colour map")
	assert_gt(int(terrain.get_tile_cache_diagnostics().get("colormap_region_updates", 0)), 0,
			"the colour pages recomposed in place")
	assert_ne(int(after.get("resident_output_hash", 0)), int(before.get("resident_output_hash", 0)),
			"the pages under the stroke show it")

	assert_true(bool(_seam.request({"kind": "undo", "path": MISSION}).get("outcome", {}).get("done", false)))
	var undone := await _settled(terrain)
	assert_eq(int(_state().get("builds", 0)), builds, "the undo is an Update")
	assert_eq(int(undone.get("resident_output_hash", 0)), int(before.get("resident_output_hash", 0)),
			"the pages as the terrain built them")


func test_a_foliage_stroke_is_an_update_its_codes_in_place() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	_look_at_corner()
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var builds := int(state.get("builds", 0))
	var terrain := _terrain(state)
	assert_not_null(terrain, "the device built its terrain")
	if terrain == null:
		return
	var data := terrain.get_terrain_data()
	# The device's foliage map at the point: the terrain's plane takes the mission's -y.
	var before := data.get_foliage_index_world(AT.x, -AT.y)

	var answer: Dictionary = _seam.request({"kind": "terrain_stroke", "path": MISSION, "stroke": {"plane": "foliage",
			"brush": {"tool": "foliage", "foliage": 77, "radius": 6}, "points": [[AT.x, AT.y]], "gesture": 0,
			"end": true}})
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer.get("outcome")))
	for _frame in 30:
		_app.pump()
		await get_tree().process_frame
	state = _state()
	assert_eq(int(state.get("builds", 0)), builds, "a foliage stroke is an Update: nothing builds again")
	assert_eq(_terrain(state), terrain, "the device's terrain stands")
	assert_eq(terrain.get_terrain_data(), data, "with its data")
	assert_ne(before, 77, "the point held another code")
	assert_eq(data.get_foliage_index_world(AT.x, -AT.y), 77, "the stroke's code in the device's foliage map")

	assert_true(bool(_seam.request({"kind": "undo", "path": MISSION}).get("outcome", {}).get("done", false)))
	for _frame in 30:
		_app.pump()
		await get_tree().process_frame
	assert_eq(int(_state().get("builds", 0)), builds, "the undo is an Update")
	assert_eq(data.get_foliage_index_world(AT.x, -AT.y), before, "the code as the terrain loaded it")
