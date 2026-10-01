extends GutTest

## The model preview headless (ADR 0046 S10p3): the editor boots with no ImGui context and
## its preview still renders the open model through the runtime's ObjectModel, read as
## JSON. The open model is drawn as it would save; the device draws the level the portable
## half picks (Auto, and a level held); a user point projects to the pixel the device's
## Camera3D puts it on; a user point's edit builds nothing, a light's builds the scene
## again; the options hold a CTRL register on the model; the camera backs away and Auto
## walks to a coarser level; unknown options are refused. S10p4: a part the model's PANM
## turns carries its marker exactly where the device's part node carries the point, at the
## clock the two share; a click's hit names the marker's record. S10p5: a drag of the
## marker lands it on the pixel the drag let go at, one undo step. S10p6: a table plays on
## the model an item pairs with it; the selected row's clip poses the device's skeleton at
## the clip clock's tick. S13 V5: the model viewport's envelope; each model its own viewport and
## device; the Shell's device cache holds four, the least recently used given up, and a viewport
## given a device again makes its picture again, its camera kept.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const ARMORY := "res://../fixtures/threedi/synth/armory.3di"
const ROCKING := "res://../fixtures/threedi/synth/house_lod0_sine_rotx.3di"
const SKINNED := "res://../fixtures/threedi/o3d/skinned.o3d"
const SKIN_CLIPS := """o3a 1
adm SKIN.adm
row anim_reset "reset"
row anim_walk_forward "walk"
clip reset
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip walk
fps 30
flags 0x1
frames 4
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0.3826834 0.9238795
 k 0 0 0.7071068 0.7071068
 k 0 0 0.3826834 0.9238795
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x1 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x2 0.9 1.7
event 0 0 0 0x2 0.9 1.7
"""

var _dirs: Array[String] = []
var _app: Node = null
## The typed seam the tests knew, over the app's request_json and query_json (S13 A5).
var _seam: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor model preview %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


func _preview() -> Dictionary:
	var parsed: Variant = JSON.parse_string(String(_app.get_model_preview_json()))
	return parsed if parsed is Dictionary else {}


func _vector(values: Variant) -> Vector3:
	var list: Array = values if values is Array else [0, 0, 0]
	return Vector3(float(list[0]), float(list[1]), float(list[2]))


func _new_project_with(model: String, name: String) -> bool:
	var dir := OS.get_cache_dir().path_join("opennova editor model preview project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	if not _seam.new_project(dir, "Model Preview Game"):
		return false
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(model))
	if bytes.is_empty() or DirAccess.make_dir_recursive_absolute(dir.path_join("models")) != OK:
		return false
	var out := FileAccess.open(dir.path_join("models").path_join(name), FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	return true


func _write(path: String, text: String) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var out := FileAccess.open(path, FileAccess.WRITE)
	out.store_string(text)
	out.close()


func _overlays(preview: Dictionary, kind: String) -> Array:
	var rows: Array = []
	for row: Variant in preview.get("items", []):
		if row is Dictionary and String(row.get("kind", "")) == kind:
			rows.append(row)
	return rows


## The model's viewport ready, a frame at a time (its device takes what it asks at each pump).
func _await_ready() -> Dictionary:
	var preview := _preview()
	for _frame in 8:
		if String(preview.get("status", "")) == "ready":
			break
		await get_tree().process_frame
		preview = _preview()
	return preview


## A copy of `model` written into the open project's models/ as `name`, the files scanned again.
func _add_model(model: String, name: String) -> void:
	var root: String = _seam.get_project_root()
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(model))
	var out := FileAccess.open(root.path_join("models").path_join(name), FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()
	_app.request_json(JSON.stringify({"kind": "rescan"}))


func _first_child(kind: String) -> int:
	var row: int = _seam.get_row_id(0)
	var ids: PackedInt64Array = _seam.get_child_records(row, kind)
	return ids[0] if ids.size() > 0 else 0


func test_preview_draws_the_open_model() -> void:
	if _app == null:
		return
	assert_false(_app.is_available(), "headless: no ImGui context")
	assert_eq(String(_preview().get("reason", "")), "no_project")

	assert_true(_new_project_with(ARMORY, "armory.3di"))
	assert_eq(String(_preview().get("reason", "")), "no_model")
	assert_true(_seam.open_document("models/armory.3di"))

	# The open model as it would save, at the level the portable half picks.
	var preview := await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_true(bool(preview.get("current", false)))
	assert_eq(int(preview.get("builds", 0)), 1)
	var model: ObjectModel = _app.get_model_preview_model()
	var camera: Camera3D = _app.get_model_preview_camera()
	assert_not_null(model)
	assert_not_null(camera)
	assert_not_null(model.get_object_data(), "the device holds the model")
	assert_eq(model.get_active_lod(), int(preview.get("body", {}).get("lod", {}).get("shown", -2)))
	assert_eq(String(preview.get("kind", "")), "model")
	assert_eq(String(preview.get("units", "")), "pixels")
	assert_true(bool(preview.get("device", {}).get("attached", false)))
	var size: Vector2i = (camera.get_viewport() as SubViewport).size
	assert_eq(size.x, int(preview.get("device", {}).get("width", 0)), "the device at the viewport's size")
	assert_eq(size.y, int(preview.get("device", {}).get("height", 0)))

	# A user point on the pixel the device's camera projects it to.
	var points: Array = _overlays(preview, "user_point")
	assert_gt(points.size(), 0)
	for point: Variant in points:
		var screen: Variant = point.get("screen")
		if screen == null:
			continue
		var at := camera.unproject_position(_vector(point.get("position")))
		assert_almost_eq(at.x, float(screen[0]), 0.5, "user point %s x" % point.get("name"))
		assert_almost_eq(at.y, float(screen[1]), 0.5, "user point %s y" % point.get("name"))

	# A user point's edit builds nothing; a light's builds the scene again.
	var serial: int = model.get_scene_build_serial()
	var user_point := _first_child("user_point")
	assert_gt(user_point, 0)
	assert_true(_seam.set_field(user_point, "position.x", float(_seam.get_field(user_point, "position.x")) + 1.0))
	preview = _preview()
	assert_true(bool(preview.get("current", false)))
	assert_eq(int(preview.get("builds", 0)), 1, "a user point is the overlays' alone")
	assert_eq(model.get_scene_build_serial(), serial, "the scene is not built again")
	var light := _first_child("light")
	assert_gt(light, 0)
	assert_true(_seam.set_field(light, "start.r", 12))
	preview = _preview()
	assert_eq(int(preview.get("builds", 0)), 2)
	assert_ne(model.get_scene_build_serial(), serial, "a light's edit builds the scene again")

	# The options: a level held and one of the model's registers (armory's FLICKER) on it.
	var registers: Array = preview.get("body", {}).get("registers", [])
	assert_gt(registers.size(), 0, "the fixture declares a CTRL register")
	var register := String(registers[0].get("name", "")) if registers.size() > 0 else "FLICKER"
	assert_true(_app.set_model_preview_options({"lod": 0, "ctrl": {register: 3}}))
	preview = _preview()
	assert_eq(int(preview.get("options", {}).get("lod", -1)), 0)
	assert_eq(int(preview.get("body", {}).get("registers", [{}])[0].get("value", 0)), 3)
	assert_eq(model.get_active_lod(), 0)
	assert_eq(int(model.get_ctrl_values().get(register, 0)), 3, str(model.get_ctrl_values()))
	assert_true(_app.set_model_preview_options({"lod": "auto", "ctrl": {}}))
	assert_false(model.get_ctrl_values().has(register), "a register let go reads 0 again")
	assert_false(_app.set_model_preview_options({"bogus": 1}))
	assert_false(_app.set_model_preview_camera({"distance": -1.0}))

	# The camera backs away: the device draws whatever Auto picks there.
	assert_true(_app.set_model_preview_camera({"distance": 5000.0}))
	preview = _preview()
	var lod: Dictionary = preview.get("body", {}).get("lod", {})
	assert_eq(model.get_active_lod(), int(lod.get("shown", -2)))
	assert_eq(int(lod.get("shown", -2)), int(lod.get("auto", -3)))
	assert_true(_app.set_model_preview_camera({"frame": true}))
	assert_lt(float(_preview().get("camera", {}).get("distance", 5000.0)), 5000.0, "framed again")


func test_markers_ride_the_parts_the_device_draws() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(ROCKING, "house.3di"))
	assert_true(_seam.open_document("models/house.3di"))
	# The ground point moved off the axis the part turns about.
	var point := _first_child("user_point")
	assert_gt(point, 0)
	assert_true(_seam.set_field(point, "position.x", 2.0))
	# The clock held at a quarter second: the device's part node and the overlay pose alike.
	assert_true(_app.set_model_preview_options({"playing": false, "time_ms": 250}))
	await get_tree().process_frame
	await get_tree().process_frame
	var preview := _preview()
	assert_eq(int(preview.get("clock", {}).get("time_ms", -1)), 250)
	var marker: Dictionary = _overlays(preview, "user_point")[0]
	var model: ObjectModel = _app.get_model_preview_model()
	var parts: Dictionary = model.get_render_part_nodes()
	assert_true(parts.has(0), str(parts.keys()))
	var node: Node3D = parts.get(0)
	# The point as authored, in the preview's space (the model's axes, x mirrored): x 2.0 in
	# the .o3d's axes is the model's z.
	var rest := Vector3(0.0, 0.0, 2.0)
	var carried: Vector3 = model.global_transform.affine_inverse() * (node.global_transform * rest)
	var at := _vector(marker.get("position"))
	assert_almost_eq(carried.x, at.x, 0.001, "x")
	assert_almost_eq(carried.y, at.y, 0.001, "y")
	assert_almost_eq(carried.z, at.z, 0.001, "z")
	assert_gt(absf(at.x), 0.01, "the part has turned the point off its rest")
	# A hit at the marker's pixel names its record.
	var screen: Array = marker.get("screen", [0, 0])
	var hit: Variant = JSON.parse_string(String(_app.model_preview_hit_json(float(screen[0]), float(screen[1]))))
	assert_true(hit is Dictionary)
	assert_eq(String(hit.get("kind", "")), "user_point")
	assert_eq(int(hit.get("id", 0)), point)
	# Dragged 30 pixels right: the marker is drawn there, the part still carrying it.
	var target := Vector2(float(screen[0]) + 30.0, float(screen[1]))
	assert_true(_app.model_preview_drag(point, "place", target.x, target.y, 0.0))
	var dragged: Dictionary = _overlays(_preview(), "user_point")[0]
	var now: Array = dragged.get("screen", [0, 0])
	assert_almost_eq(float(now[0]), target.x, 0.5)
	assert_almost_eq(float(now[1]), target.y, 0.5)
	assert_false(_app.model_preview_drag(point, "twist", target.x, target.y, 0.0), "an unknown handle")
	# One undo step takes it back.
	_seam.undo()
	var undone: Array = _overlays(_preview(), "user_point")[0].get("screen", [0, 0])
	assert_almost_eq(float(undone[0]), float(screen[0]), 0.5)


func test_a_table_plays_on_its_rig() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor model preview rig %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir.path_join("project"), "Rig Game"))
	var source := dir.path_join("source")
	_write(source.path_join("skinned.o3d"), FileAccess.get_file_as_string(ProjectSettings.globalize_path(SKINNED)))
	_write(source.path_join("skin.o3a"), SKIN_CLIPS)
	_app.request_json(JSON.stringify({"kind": "import_files", "imports": [
		{"path": source.path_join("skinned.o3d")}, {"path": source.path_join("skin.o3a")}]}))
	_write(dir.path_join("project/defs/items.def"),
			"begin \"Skinned Thing\"\nid 100200\ntype building\ngraphic skinned\nanim_def skin\nend\n")
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.open_document("anims/SKIN.adm"))
	var walk: int = _seam.find_record("anim_walk_forward")
	assert_gt(walk, 0)
	assert_true(_seam.select_record(walk))
	assert_true(_app.set_model_preview_options({"playing": false, "clip_ticks": 0}))
	var preview := await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	var animation: Dictionary = preview.get("body", {}).get("animation", {})
	assert_eq(String(animation.get("model", "")).to_lower(), "skinned.3di")
	assert_true(bool(animation.get("rig", false)))
	assert_eq(String(animation.get("key", "")), "anim_walk_forward")
	assert_eq(animation.get("events", []).size(), 2)
	await get_tree().process_frame
	var model: ObjectModel = _app.get_model_preview_model()
	assert_true(model.has_skeleton(), "the rig is bound to the skinned model")
	assert_eq(model.get_active_body_clip(), "anim_walk_forward")
	var skeleton := model.get_skeleton()
	var at_rest := skeleton.get_bone_pose_rotation(0)
	# A quarter of the way in, the root has turned.
	assert_true(_app.set_model_preview_options({"clip_ticks": 8}))
	await get_tree().process_frame
	var turned := skeleton.get_bone_pose_rotation(0)
	assert_gt(at_rest.angle_to(turned), 0.05, "the clip poses the skeleton at the clip clock")
	assert_eq(int(_preview().get("body", {}).get("animation", {}).get("ticks", -1)), 8)
	assert_eq(int(_preview().get("clock", {}).get("ticks", -1)), 8, "the preview clock's ticks")


## S13 V5: each open model its own viewport and its own device: the first again keeps its device
## and its picture (no build), its camera its own.
func test_two_models_get_their_own_devices() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(ARMORY, "armory.3di"))
	_add_model(ARMORY, "second.3di")
	assert_true(_seam.open_document("models/armory.3di"))
	var first := await _await_ready()
	assert_eq(String(first.get("status", "")), "ready", str(first))
	var first_model: ObjectModel = _app.get_model_preview_model()
	assert_true(_app.set_model_preview_camera({"distance": 30.0}))
	assert_true(_seam.open_document("models/second.3di"))
	var second := await _await_ready()
	assert_eq(String(second.get("path", "")), "models/second.3di", str(second))
	var second_model: ObjectModel = _app.get_model_preview_model()
	assert_not_null(second_model)
	assert_ne(second_model, first_model, "the second model on a device of its own")
	assert_ne(float(second.get("camera", {}).get("distance", 30.0)), 30.0, "its own camera, framed on it")
	assert_true(_seam.open_document("models/armory.3di"))
	var again := await _await_ready()
	assert_eq(_app.get_model_preview_model(), first_model, "the first's device kept")
	assert_eq(int(again.get("builds", 0)), int(first.get("builds", -1)), "its picture kept: no build")
	assert_almost_eq(float(again.get("camera", {}).get("distance", 0.0)), 30.0, 0.001, "its camera kept")


## S13 V5: the Shell's devices hold four: a fifth model previewed gives up the least recently used
## (path, kind)'s device, its viewport detached with its state kept; previewed again, it is given a
## device that makes its picture again (a build), at the camera it kept.
func test_the_device_cache_holds_four() -> void:
	if _app == null:
		return
	assert_true(_new_project_with(ARMORY, "m0.3di"))
	for i in range(1, 5):
		_add_model(ARMORY, "m%d.3di" % i)
	assert_true(_seam.open_document("models/m0.3di"))
	var first := await _await_ready()
	assert_eq(int(first.get("builds", 0)), 1, str(first))
	assert_true(_app.set_model_preview_camera({"yaw": 1.25, "distance": 12.0}))
	var first_model: ObjectModel = _app.get_model_preview_model()
	for i in range(1, 5):
		assert_true(_seam.open_document("models/m%d.3di" % i))
		assert_eq(String((await _await_ready()).get("status", "")), "ready")
	await get_tree().process_frame
	assert_false(is_instance_valid(first_model), "the least recently used device given up, its nodes freed")
	assert_true(_seam.open_document("models/m0.3di"))
	var again := await _await_ready()
	assert_true(bool(again.get("device", {}).get("attached", false)), str(again))
	assert_eq(int(again.get("builds", 0)), 2, "a device attached again makes the picture again")
	assert_almost_eq(float(again.get("camera", {}).get("yaw", 0.0)), 1.25, 0.001, "the camera it kept")
	assert_almost_eq(float(again.get("camera", {}).get("distance", 0.0)), 12.0, 0.001)
	assert_not_null(_app.get_model_preview_model(), "a device of its own again")
