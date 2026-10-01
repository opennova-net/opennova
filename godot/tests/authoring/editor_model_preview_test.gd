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
## given a device again makes its picture again, its camera kept. S13 V6: the device builds its
## picture over frames (its textures, its meshes, the scene, the pose), so a viewport reads `loading`
## before `ready` and every test awaits `ready` after what builds again; a JO-sized model loads over
## several frames within the build budget, the last picture kept until the scene unit swaps the new
## one in, one unit a frame at a budget of 0.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
## The JO-sized model (S13 V6), as large as the 967 the game ships come near their 99th percentile
## (six levels, 4,304 triangles at the first and 10,409 in all, 16 materials, 29 texture rows, 18
## textures): the materials (a texture each, every third a detail texture too), the parts, the quads
## across each part's sheet at the first level (halved at each level after), the levels, the
## textures' side in pixels.
const LARGE_MATERIALS := 16
const LARGE_PARTS := 12
const LARGE_CELLS := 16
const LARGE_LEVELS := 5
const LARGE_SIDE := 256
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
	return _seam.settle() # a Rescan steps across pumps (S13 A3)


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


## The model's viewport ready, a frame at a time (its device takes what it asks at each pump and
## builds its picture over the frames, S13 V6: `loading` until it is built).
func _await_ready() -> Dictionary:
	var preview := _preview()
	for _frame in 600:
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
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")


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
	assert_false(bool(preview.get("device", {}).get("canvas_sized", true)), "headless: no canvas sizes it")
	# Headless, the device's size is the viewport's to set: a `device` object, never flat keys.
	assert_true(_app.set_model_preview_camera({"device": {"width": 320, "height": 240}}))
	assert_eq((camera.get_viewport() as SubViewport).size, Vector2i(320, 240))
	assert_eq(int(_preview().get("device", {}).get("width", 0)), 320)
	assert_false(_app.set_model_preview_camera({"width": 640}), "the device's size is the device's member")
	assert_ne(String(_app.get_preview_error()), "", "the refusal says why")
	assert_true(_app.set_model_preview_camera({"device": {"width": size.x, "height": size.y}}))
	preview = _preview()

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
	# Built over the frames that follow (S13 V6): loading first, the scene swapped in once built.
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	preview = await _await_ready()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	assert_eq(int(preview.get("device", {}).get("build", {}).get("generation", 0)), 2)
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
	assert_eq(String((await _await_ready()).get("status", "")), "ready")
	# The ground point moved off the axis the part turns about.
	var point := _first_child("user_point")
	assert_gt(point, 0)
	assert_true(_seam.set_field(point, "position.x", 2.0))
	# The clock held at a quarter second: the device's part node and the overlay pose alike.
	assert_true(_app.set_model_preview_options({"clock": {"playing": false, "time_ms": 250}}))
	await get_tree().process_frame
	await get_tree().process_frame
	var preview := _preview()
	assert_eq(int(preview.get("clock", {}).get("time_ms", -1)), 250)
	# The clock's members are the clock's (its rate among them), never the options'.
	assert_false(_app.set_model_preview_options({"time_ms": 250}))
	assert_true(_app.set_model_preview_options({"clock": {"rate": 2.0}}))
	assert_eq(float(_preview().get("clock", {}).get("rate", 0.0)), 2.0)
	assert_true(_app.set_model_preview_options({"clock": {"rate": 1.0}}))
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
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	assert_true(_seam.open_document("anims/SKIN.adm"))
	var walk: int = _seam.find_record("anim_walk_forward")
	assert_gt(walk, 0)
	assert_true(_seam.select_record(walk))
	assert_true(_app.set_model_preview_options({"clock": {"playing": false, "ticks": 0}}))
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
	assert_true(_app.set_model_preview_options({"clock": {"ticks": 8}}))
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


## A 32-bit TGA `side` pixels square (its pixels one colour): a texture the game decodes itself.
func _write_tga(path: String, side: int) -> void:
	var image := Image.create(side, side, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.45, 0.55, 0.35, 1.0))
	var header := PackedByteArray([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		side & 0xFF, side >> 8, side & 0xFF, side >> 8, 32, 8])
	var out := FileAccess.open(path, FileAccess.WRITE)
	out.store_buffer(header)
	out.store_buffer(image.get_data())
	out.close()


## A sheet of `cells` x `cells` quads at x = `x` facing +x (mission axes: y left, z up), two metres
## square, as a strip's vertices and triangles (counter-clockwise about its normal).
func _sheet(lines: PackedStringArray, x: float, cells: int) -> void:
	for row in cells + 1:
		for column in cells + 1:
			var u := float(column) / cells
			var v := float(row) / cells
			lines.append("v %.4f %.4f %.4f 1 0 0 %.4f %.4f" % [x, -1.0 + 2.0 * u, 2.0 * v, u, 1.0 - v])
	for row in cells:
		for column in cells:
			var a := row * (cells + 1) + column
			var b := a + 1
			var c := a + cells + 2
			var d := a + cells + 1
			lines.append("t %d %d %d" % [a, b, c])
			lines.append("t %d %d %d" % [a, c, d])


## A JO-sized model's source written into `dir` as the Blender add-on writes a scene (an .o3d), its
## textures beside it: `materials` materials, each with a diffuse texture and every third a detail
## texture too (32-bit TGAs `side` pixels square); `levels` levels of `parts` parts, a part a sheet
## of `cells` x `cells` quads at the first level, half as many across at each level after; a light
## and a user point. Answers the textures' file names.
func _write_large_model(dir: String, stem: String, materials: int, parts: int, cells: int, levels: int,
		side: int) -> PackedStringArray:
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var textures := PackedStringArray()
	var lines := PackedStringArray(["o3d 1", "model %s" % stem.to_upper()])
	for index in materials:
		var detail := index % 3 == 2
		lines.append("material %s" % ("FF_MT_OP" if detail else "FF_ST_OP"))
		var diffuse := "%s%02d.tga" % [stem, index]
		lines.append("texture %s 1 0 0 0" % diffuse)
		textures.append(diffuse)
		if detail:
			var second := "%s%02dd.tga" % [stem, index]
			lines.append("texture %s 2 0 0 0" % second)
			textures.append(second)
	for level in levels:
		# The level drawn past a threshold that halves at each level, the last at any distance.
		lines.append("lod %d bldg" % (0 if level == levels - 1 else 400 >> level))
		for part in parts:
			lines.append("part 0 %d 0 0" % (part * 3))
			lines.append("strip %d 0" % (part % materials))
			_sheet(lines, float(part * 3), maxi(cells >> level, 1))
	lines.append("light 0 1 0 1.5 0 4 0 0 0 255 200 150 255 200 150 0")
	lines.append("userpoint top 0 0 3 0 0 1 0 71")
	var out := FileAccess.open(dir.path_join(stem + ".o3d"), FileAccess.WRITE)
	out.store_string("\n".join(lines) + "\n")
	out.close()
	for texture in textures:
		_write_tga(dir.path_join(texture), side)
	return textures


## S13 V6: a JO-sized model builds over frames. Opened, its viewport is `loading` at once (the build
## begun, its units planned: textures, meshes, the scene, the pose), its progress never going back,
## then `ready`; at the editor's build budget each frame's units take no more than the budget and one
## unit's cost (measured, asserted loosely: the longest frame within the budget plus the longest unit
## and a millisecond). At a budget of 0 each frame runs one unit, so the build takes as many frames
## as it has units; until its scene unit the device's ObjectModel holds the last scene (the last
## picture kept, nothing half built in it); a light's edit at a unit begins the next generation anew.
func test_a_large_model_builds_over_frames() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor model preview large %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir.path_join("project"), "Large Game"))
	var source := dir.path_join("source")
	var textures := _write_large_model(source, "jolarge", LARGE_MATERIALS, LARGE_PARTS, LARGE_CELLS, LARGE_LEVELS,
			LARGE_SIDE)
	var imports: Array = [{"path": source.path_join("jolarge.o3d")}]
	for texture in textures:
		imports.append({"path": source.path_join(texture)})
	var imported: Dictionary = _seam.request({"kind": "import_files", "imports": imports})
	assert_true(bool(imported.get("ok", false)), str(imported))
	assert_true(_seam.settle(), "the import steps across pumps (S13 A3)")
	assert_true(_seam.open_document("models/jolarge.3di"))

	# The editor's budget: loading at once, its progress rising, then ready.
	var budget_ms: int = _app.get_build_budget_ms()
	assert_gt(budget_ms, 0)
	var preview := _preview()
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	var total := int(preview.get("progress", {}).get("total", 0))
	assert_gt(total, textures.size(), "a unit a texture, then the meshes, the scene and the pose")
	var done := 0
	var frames := 0
	while String(preview.get("status", "")) == "loading" and frames < 600:
		var now := int(preview.get("progress", {}).get("done", 0))
		assert_true(now >= done, "the progress never goes back")
		done = now
		await get_tree().process_frame
		frames += 1
		preview = _preview()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	var build: Dictionary = preview.get("device", {}).get("build", {})
	gut.p("the JO-sized model at %d ms a frame: %d frames awaited, its build %s" % [budget_ms, frames, str(build)])
	assert_eq(int(build.get("done", 0)), total, str(build))
	assert_gt(int(build.get("frames", 0)), 0, str(build))
	var frame_us := int(build.get("frame_us", 0))
	var unit_us := int(build.get("unit_us", 0))
	assert_lte(frame_us, budget_ms * 1000 + unit_us + 1000,
			"no frame's units past the budget by more than one unit: %s" % str(build))
	var model: ObjectModel = _app.get_model_preview_model()
	assert_not_null(model)
	assert_not_null(model.get_object_data(), "the device holds the model")

	# A budget of 0: one unit a frame. Until the scene unit the ObjectModel holds the last scene.
	_app.set_build_budget_ms(0)
	var serial: int = model.get_scene_build_serial()
	var light := _first_child("light")
	assert_gt(light, 0)
	assert_true(_seam.set_field(light, "start.r", 12))
	preview = _preview()
	assert_eq(String(preview.get("status", "")), "loading", str(preview))
	assert_eq(int(preview.get("builds", 0)), 2)
	done = 0
	for _frame in 3:
		await get_tree().process_frame
		preview = _preview()
		var now := int(preview.get("progress", {}).get("done", 0))
		assert_true(now - done <= 1, "one unit a frame: %d after %d" % [now, done])
		done = now
		assert_eq(String(preview.get("progress", {}).get("label", "")), "textures", str(preview))
		assert_eq(model.get_scene_build_serial(), serial, "the last scene kept while the textures decode")
	assert_gt(done, 1, "a unit each frame")
	# Another edit while it builds: the next generation begun anew from its first unit.
	assert_true(_seam.set_field(light, "start.r", 13))
	preview = _preview()
	assert_eq(int(preview.get("builds", 0)), 3)
	assert_eq(int(preview.get("device", {}).get("build", {}).get("generation", 0)), 3)
	assert_eq(int(preview.get("progress", {}).get("done", -1)), 0, "begun anew")
	frames = 0
	while String(preview.get("status", "")) == "loading" and frames < total + 10:
		await get_tree().process_frame
		frames += 1
		preview = _preview()
	assert_eq(String(preview.get("status", "")), "ready", str(preview))
	build = preview.get("device", {}).get("build", {})
	gut.p("the JO-sized model at 0 ms a frame: %d frames awaited, its build %s" % [frames, str(build)])
	assert_eq(int(build.get("frames", 0)), total, "one unit a frame, as many frames as units: %s" % str(build))
	assert_eq(int(build.get("generation", 0)), 3, str(build))
	assert_ne(model.get_scene_build_serial(), serial, "the scene built again once its unit ran")
