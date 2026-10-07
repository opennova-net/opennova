extends GutTest

## The jumps' keys and menus on the wire (ADR 0046 DI-18, "Shortcuts and context jumps"): what each shortcut and
## each right-click menu entry raises, through the editor's own request_json and query_json, over a minted
## project holding the fixture's item catalog, the minted mission and the synth models its items draw. Go to file
## (Ctrl+P) and Go to name (Ctrl+T) are the finder's workspace part in their scope, their lists the
## project_search query's of that scope; a hit gone to is an open_document, a step of the navigation history.
## Find usages (Shift+F12, a menu's Find usages) is the finder in its usages scope over a file or a record, its
## list the usages query's with that locator. Go to definition (F12, Ctrl+click, a menu's Go to item) is the
## open_document of the reference's target (reference_targets). An item record's Place in mission is the
## definition viewport's place_in_mission command, arming the mission's Place tool; the mission view's Place
## another a set_viewport of its tool and item. Headless: no ImGui context draws, so the keys and the menus
## themselves are the editor_ui ctest's (tests/editor_ui/shortcuts_test.cpp), over the null backend.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const FIXTURES := "res://../fixtures/"
const ITEMS := "defs/items.def"
const MISSION := "missions/synth_logic.bms"

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor jumps %d" % Time.get_ticks_usec())
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
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "writes " + path)
	if file:
		file.store_buffer(bytes)
		file.close()


func _copy_fixture(source: String, target: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FIXTURES + source))
	assert_false(bytes.is_empty(), "the fixture is available: " + source)
	_write(target, bytes)


## A new project holding the fixture's catalog, the minted mission and the models its items draw, scanned,
## the catalog and the mission open.
func _open_project() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor jumps project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	if not _seam.new_project(dir, "Jumps"):
		return false
	_seam.create_missing_files()
	var root: String = _seam.get_project_root()
	_copy_fixture("def/items.def", root.path_join(ITEMS))
	_copy_fixture("bms/synth_logic.bms", root.path_join(MISSION))
	_copy_fixture("bms/synth_logic.bin", root.path_join("missions/synth_logic.bin"))
	for name in ["pump.3di", "armory.3di", "shed.3di"]:
		_copy_fixture("threedi/synth/" + name, root.path_join("models").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps")
	return _seam.open_document(MISSION) and _seam.open_document(ITEMS)


func _finder() -> Dictionary:
	return _seam.state(["workspace"]).get("workspace", {}).get("project_find", {})


func _active() -> String:
	return String(_seam.state(["documents"]).get("documents", {}).get("active", ""))


## The record of `path` defining `symbol` (an item's id): its query answer.
func _record(path: String, symbol: String) -> Dictionary:
	return _seam.query("record", {"path": path, "symbol": symbol})


func test_go_to_file_and_name() -> void:
	if _app == null:
		return
	assert_true(_open_project(), "the jumps project")
	# Ctrl+P: the finder in its files scope, its list the files alone, the first gone to.
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"project_find": {"open": true, "scope": "files"}}}))
	var finder := _finder()
	assert_true(bool(finder.get("open", false)), str(finder))
	assert_eq(String(finder.get("scope", "")), "files")
	var hits: Array = _seam.query("project_search", {"text": "pump", "scope": "files"}).get("hits", [])
	assert_false(hits.is_empty(), "files hold pump")
	if hits.is_empty():
		return
	assert_eq(String(hits[0].get("file", "")), "models/pump.3di", "the model itself first")
	for hit: Variant in hits:
		assert_eq(String((hit as Dictionary).get("kind", "")), "file", "the files alone")
	assert_true(_seam.done({"kind": "open_document", "path": String(hits[0]["file"])}), "the hit gone to")
	assert_eq(_active(), "models/pump.3di")
	var back: Array = _seam.state(["navigation"]).get("navigation", {}).get("back", [])
	assert_false(back.is_empty(), "a step of the navigation history")
	# Ctrl+T: the names alone, the rifleman by its catalog's name; its record gone to.
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"project_find": {"scope": "names"}}}))
	assert_eq(String(_finder().get("scope", "")), "names")
	assert_eq(String(_finder().get("text", "?")), "", "another scope starts afresh")
	hits = _seam.query("project_search", {"text": "rifleman", "scope": "names"}).get("hits", [])
	assert_false(hits.is_empty(), "a name holds rifleman")
	if hits.is_empty():
		return
	assert_string_contains(String(hits[0].get("words", "")), "Rifleman")
	var target: Dictionary = hits[0]
	assert_true(_seam.done({"kind": "open_document", "path": String(target.get("file", "")),
			"locator": String(target.get("locator", "")), "field": String(target.get("field", ""))}))
	assert_eq(_active(), ITEMS, "the catalog, at the rifleman")
	var refused: Dictionary = _seam.query("project_search", {"text": "pump", "scope": "everything"})
	assert_string_contains(String(refused.get("error", "")), "no scope")


func test_find_usages() -> void:
	if _app == null:
		return
	assert_true(_open_project(), "the jumps project")
	var pump := _record(ITEMS, "106100")
	var locator := String(pump.get("locator", ""))
	assert_false(locator.is_empty(), str(pump))
	# Shift+F12 over the pump's record: the finder on its uses, the usages query's with its locator.
	var change := {"project_find": {"open": true, "scope": "usages", "path": ITEMS, "locator": locator}}
	assert_true(_seam.done({"kind": "set_workspace", "workspace": change}))
	var finder := _finder()
	assert_eq(String(finder.get("scope", "")), "usages")
	assert_eq(String(finder.get("path", "")), ITEMS)
	assert_eq(String(finder.get("locator", "")), locator)
	var uses: Dictionary = _seam.query("usages", {"path": ITEMS, "locator": locator})
	assert_eq(int(uses.get("count", -1)), 3, "the mission's three pumps")
	for edge: Variant in uses.get("edges", []):
		assert_eq(String((edge as Dictionary).get("source", "")), MISSION)
	# A model's uses: the items drawing it.
	var drawn: Dictionary = _seam.query("usages", {"path": "models/pump.3di"})
	assert_gt(int(drawn.get("count", 0)), 0, "items draw the pump")
	var refused: Dictionary = _seam.query("usages", {"locator": locator})
	assert_string_contains(String(refused.get("error", "")), "a locator names a record")


func test_go_to_definition_and_place() -> void:
	if _app == null:
		return
	assert_true(_open_project(), "the jumps project")
	# The walker's item (F12 with it selected, Ctrl+click its value, Go to item): its record in the catalog.
	var walker_id := 0
	for row: Variant in _seam.every("document", "rows", {"path": MISSION}):
		var record: Dictionary = _seam.query("record", {"id": int((row as Dictionary).get("id", 0)), "path": MISSION})
		for field: Variant in record.get("fields", []):
			if String((field as Dictionary).get("id", "")) == "item" and int((field as Dictionary).get("value", 0)) == 106102:
				walker_id = int(record.get("id", 0))
		if walker_id != 0:
			break
	assert_ne(walker_id, 0, "the walker")
	var targets: Array = _seam.query("reference_targets", {"path": MISSION, "id": walker_id, "field": "item"}).get("targets", [])
	assert_eq(targets.size(), 1, str(targets))
	if targets.is_empty():
		return
	var target: Dictionary = targets[0]
	assert_eq(String(target.get("file", "")), ITEMS)
	assert_true(_seam.done({"kind": "open_document", "path": ITEMS, "locator": String(target.get("locator", "")),
			"field": String(target.get("field", ""))}))
	assert_eq(_active(), ITEMS)
	# Place in mission over the pump's record: the definition viewport's command, the mission's Place tool armed.
	var pump := _record(ITEMS, "106100")
	var answer: Dictionary = _seam.request({"kind": "edit_in_viewport", "path": ITEMS,
			"command": {"name": "place_in_mission", "kind": "definition", "ids": [int(pump.get("row", 0))]}})
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
	assert_string_contains(String(answer.get("status", "")), "Place is armed in synth_logic.bms")
	assert_eq(_active(), MISSION, "the mission made active")
	var options: Dictionary = _seam.query("viewport", {"op": "state", "path": MISSION, "kind": "mission"}).get("options", {})
	assert_eq(String(options.get("tool", "")), "place")
	assert_eq(int(options.get("item", 0)), 106100)
	# Two rows: refused, naming why.
	var armory := _record(ITEMS, "106101")
	var refused: Dictionary = _seam.request({"kind": "edit_in_viewport", "path": ITEMS,
			"command": {"name": "place_in_mission", "kind": "definition",
				"ids": [int(pump.get("row", 0)), int(armory.get("row", 0))]}})
	assert_false(bool(refused.get("outcome", {}).get("done", true)), "two rows refused")
	assert_string_contains(str(refused.get("outcome", {}).get("findings", [])), "one item record")
	# The mission view's Place another: a set_viewport of its tool and the entity's item.
	assert_true(_seam.done({"kind": "set_viewport", "path": MISSION,
			"viewport": {"kind": "mission", "options": {"tool": "place", "item": 106102}}}))
	options = _seam.query("viewport", {"op": "state", "path": MISSION, "kind": "mission"}).get("options", {})
	assert_eq(int(options.get("item", 0)), 106102)
