extends GutTest

## The terrain brush through the editor's wire seam, headless (ADR 0046 S25, In the views): the Terrain pane and the
## brush set on the workspace and read back by the terrain query (the brush live, the files it edits as the game
## resolves them for the mission); a stamp of the Tiles tool dropped on the mission's map lays a tile in the mission's
## .til, opened quietly beside the mission, which stays active; the mission's Undo takes the stamp back and names the
## .til it stepped, its Redo makes it again; the brush put down (tool none), a drop of a tile is the map's to refuse.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION_FIXTURE := "res://../fixtures/bms/synth_logic.bms"
const FIXTURES := "res://../fixtures/"
const MISSION := "missions/synth_logic.bms"
const TILES := "missions/synth_logic.til"

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor terrain brush %d" % Time.get_ticks_usec())
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


## A new project holding the minted mission on its terrain (Tmap: its .trn, its heights and its two maps) and its
## environment, scanned, the mission open.
func _open_mission() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor terrain brush project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Terrain Brush"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join(MISSION), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION_FIXTURE)))
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	# The overcast table every mission's environment load reads (env::kOvercastFile; the game notes it
	# missing where a project lacks it), here present and empty: no keyframe, as the load with none.
	_write(root.path_join("env").path_join("overcast.def"), PackedByteArray())
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return _seam.open_document(MISSION)


func _terrain(op: String, args := {}) -> Dictionary:
	var params: Dictionary = args.duplicate()
	params["op"] = op
	params["path"] = MISSION
	return _seam.query("terrain", params)


func _open_paths() -> Array:
	var paths: Array = []
	for row: Variant in _seam.state(["documents"]).get("documents", {}).get("open", []):
		paths.append(String((row as Dictionary).get("path", "")))
	return paths


func _tiles() -> int:
	var answer := _terrain("tiles")
	return int(answer.get("count", -1)) if not answer.has("error") else -1


func test_the_pane_and_the_brush_on_the_wire() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"document": {"path": MISSION, "terrain": true},
			"terrain_brush": {"tool": "raise", "radius": 12.5, "strength": 0.5}}}))
	var brush := _terrain("brush")
	assert_false(brush.has("error"), str(brush))
	assert_true(bool(brush.get("pane", false)), "the Terrain pane shown")
	assert_true(bool(brush.get("live", false)), "the views brush with it")
	assert_eq(String(brush.get("plane", "")), "heights")
	assert_almost_eq(float(brush.get("brush", {}).get("radius", 0)), 12.5, 0.001)
	# The files the brush edits for the mission, as the game resolves them.
	var family := _terrain("family")
	assert_false(family.has("error"), str(family))
	assert_true(String(family.get("planes", {}).get("heights", {}).get("path", "")).ends_with("Tmap.cpt"), str(family))
	assert_eq(String(family.get("tiles", {}).get("path", "")), TILES, str(family.get("tiles")))
	# A refusal for a member out of its range, nothing of the part set.
	var refused: Dictionary = _seam.request({"kind": "set_workspace", "workspace": {"terrain_brush": {"radius": 9000}}})
	assert_false(bool(refused.get("outcome", {}).get("done", true)), str(refused))
	assert_almost_eq(float(_terrain("brush").get("brush", {}).get("radius", 0)), 12.5, 0.001)


func test_a_stamp_on_the_map_and_the_missions_undo() -> void:
	if _app == null:
		return
	assert_true(_open_mission())
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"document": {"path": MISSION, "terrain": true, "map": true},
			"terrain_brush": {"tool": "stamp", "cell": 3}}}))
	var before := _tiles()
	var stamp: Dictionary = _seam.request({"kind": "edit_in_viewport", "path": MISSION,
			"drop": {"reference": "tile", "name": "3", "at": [40, 40], "kind": "map"}})
	assert_true(bool(stamp.get("outcome", {}).get("done", false)), str(stamp))
	assert_true(_open_paths().has(TILES), "the .til opened beside the mission: %s" % str(_open_paths()))
	assert_eq(String(_seam.state(["documents"]).get("documents", {}).get("active", "")), MISSION, "the mission stays active")
	var after := _tiles()
	assert_eq(after, maxi(before, 0) + 1, "a tile laid")
	# The mission's Undo takes the stamp back, naming the .til it stepped; its Redo makes it again.
	var undone: Dictionary = _seam.request({"kind": "undo", "path": MISSION})
	assert_true((undone.get("outcome", {}).get("documents", []) as Array).has(TILES), str(undone.get("outcome")))
	assert_eq(_tiles(), after - 1)
	_seam.request({"kind": "redo", "path": MISSION})
	assert_eq(_tiles(), after)
	# The brush put down: a tile's drop is no longer the brush's.
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"terrain_brush": {"tool": "none"}}}))
	assert_false(bool(_terrain("brush").get("live", true)))
