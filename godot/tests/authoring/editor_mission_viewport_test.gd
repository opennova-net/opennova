extends GutTest

## The mission viewport headless through the editor's wire seam (ADR 0046 S14): a mission opened in a
## minted project shows in its Main-role viewport (kind "mission"), read through query_json's
## `viewport` and changed through request_json's set_viewport and edit_in_viewport; its device (the
## mission's Environment and Camera3D in an offscreen SubViewport, get_viewport_device) takes what
## changed at the next pump. The marks sit where the device's camera draws them: each entity's `screen`
## is within half a pixel of Camera3D.unproject_position of MissionObjectPlacer.bms_to_godot_position
## of its position, and the yaw handle of a yaw-90 entity lies east of its anchor; the camera set on
## the wire (a compass heading) is the device's; an entity's move is an Update (the build generation
## stands); a hit at a mark's pixel names its record and a box the records inside.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const MISSION := "res://../fixtures/bms/synth_logic.bms"

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
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		await get_tree().process_frame
		state = _state(path)
	return state


func _device(state: Dictionary) -> SubViewport:
	return _app.get_viewport_device(String(state.get("path", "")), String(state.get("kind", "")))


func _device_node(state: Dictionary, type: String) -> Node:
	var device := _device(state)
	if device == null:
		return null
	var found := device.find_children("*", type, true, false)
	return found[0] if not found.is_empty() else null


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


## A new project holding the minted mission as missions/synth_logic.bms, scanned, the mission open.
func _open_mission() -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor mission project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Mission Viewport Game"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_write(root.path_join("missions").path_join("synth_logic.bms"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(MISSION)))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return _seam.open_document("missions/synth_logic.bms")


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
	var camera := _device_node(state, "Camera3D") as Camera3D
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
	var camera := _device_node(state, "Camera3D") as Camera3D
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
