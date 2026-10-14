extends GutTest

## S25 (ADR 0046, In the views): the terrain brush over the mission's 3D view in a window, the editor's ImGui pass
## attached and the Document window's canvas drawing the mission's device. A drag of the Raise brush across the
## picture's middle raises the ground there: the terrain query's height at the stroke's middle higher than before, the
## device's terrain patched in place (no build of the picture), and the window's own picture over the stroke other
## than before. The drag is the wire's (`edit_in_viewport`'s brush drag, a sample at each picture pixel under one
## gesture, as the canvas's brush half raises a stroke's samples): imgui-godot reads ImGui's pointer from the OS's each
## frame (DisplayServer.mouse_get_position), which no test may move (editor_mission_logic_device_test.gd). A
## tests/windowed/ script, run by `scripts/test_godot.sh --suite core --windowed`.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const FIXTURES := "res://../fixtures/"
const TERRAIN_TEXTURES := ["mnml_c", "mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1", "mnml_t"]
const MISSION_PATH := "missions/synth_logic.bms"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "terrain device", _dirs, 3)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	await EditorFixture.teardown(self, _dirs)


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


## A texture `size` a side, grey, as an uncompressed true-colour TGA (the runtime fixture's minting).
func _tga(size: int) -> PackedByteArray:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.5, 0.5, 0.5, 1.0))
	var header := PackedByteArray()
	header.resize(18)
	header[2] = 2
	header.encode_u16(12, size)
	header.encode_u16(14, size)
	header[16] = 32
	header[17] = 0x28
	return header + image.get_data()


## A project holding the mission on its terrain (Tmap, its maps and textures), its environment and the items its
## picture draws; scanned.
func _mint() -> void:
	var dir := OS.get_cache_dir().path_join("opennova editor terrain device project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Terrain Device"))
	assert_eq(_seam.create_missing_files(), 0)
	var root: String = _seam.get_project_root()
	_copy_fixture("bms/synth_logic.bms", root.path_join(MISSION_PATH))
	for name in ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]:
		_copy_fixture("terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	var small := _tga(32)
	var large := _tga(2048)
	for name in TERRAIN_TEXTURES:
		_write(root.path_join("terrain").path_join(name + ".tga"), large if name == "mnml_c" or name == "mnml_d1" else small)
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		_copy_fixture("env/" + name, root.path_join("env").path_join(name))
	_copy_fixture("def/items.def", root.path_join("defs").path_join("items.def"))
	for name in ["pump.3di", "armory.3di", "shed.3di"]:
		_copy_fixture("threedi/synth/" + name, root.path_join("models").path_join(name))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": MISSION_PATH, "kind": "mission", "limit": 1})


func _frames(count: int) -> void:
	for _frame in count:
		await get_tree().process_frame


## One sample of the brush's drag at the picture's pixel `at` (the wire's, under `gesture`; 0 begins one): its gesture.
## Sent as a client streams a stroke's samples, a frame or two apart, never waiting on the work a sample starts (the
## first stroke's bake state, made in the background): the editor pumps between them.
func _drag(at: Vector2, gesture: int, end: bool) -> int:
	var sent := {"kind": "edit_in_viewport", "path": MISSION_PATH,
			"drag": {"id": 0, "handle": "brush", "to": [at.x, at.y], "gesture": gesture, "end": end, "kind": "mission"}}
	var answer: Dictionary = EditorSeam.parsed(String(_app.call("request_json", JSON.stringify(sent))))
	_app.pump()
	assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
	await _frames(2)
	return int(answer.get("outcome", {}).get("gesture", 0))


## The mission point under the picture's pixel, as the viewport's hit reads it (the ground under it).
func _ground_at(pixel: Vector2) -> Array:
	var hit: Dictionary = _seam.query("viewport", {"op": "hit", "path": MISSION_PATH, "kind": "mission", "x": pixel.x, "y": pixel.y})
	var ground: Dictionary = hit.get("ground", {})
	var at: Array = ground.get("at", [])
	return at


## How far two pictures differ over a square about `centre`: the sum of each texel's largest channel difference.
func _difference(a: Image, b: Image, centre: Vector2i, half: int) -> float:
	var total := 0.0
	for y in range(maxi(0, centre.y - half), mini(a.get_height(), centre.y + half)):
		for x in range(maxi(0, centre.x - half), mini(a.get_width(), centre.x + half)):
			var p := a.get_pixel(x, y)
			var q := b.get_pixel(x, y)
			total += maxf(absf(p.r - q.r), maxf(absf(p.g - q.g), absf(p.b - q.b)))
	return total


func test_a_drag_of_the_raise_brush_raises_the_hill() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	_mint()
	assert_true(_seam.open_document(MISSION_PATH))
	var state := _state()
	for _frame in 900:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)):
			break
		await get_tree().process_frame
		state = _state()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	var device: SubViewport = _app.get_viewport_device(MISSION_PATH, "mission")
	assert_not_null(device)
	if device == null:
		return
	# Straight down over the target, the Terrain pane shown with Raise.
	assert_true(_seam.done({"kind": "edit_in_viewport", "path": MISSION_PATH, "command": {"name": "top", "kind": "mission"}}))
	assert_true(_seam.done({"kind": "set_workspace", "workspace": {"document": {"path": MISSION_PATH, "terrain": true},
			"terrain_brush": {"tool": "raise", "radius": 40, "strength": 1.0, "falloff": 0.5}}}))
	await _frames(40)
	var size := Vector2(device.size)
	assert_gt(size.x, 64.0, "the canvas draws the picture")
	var middle := size * 0.5
	var under := _ground_at(middle)
	assert_eq(under.size(), 3, "the ground under the picture's middle")
	if under.size() != 3:
		return
	var sampled: Dictionary = _seam.query("terrain", {"path": MISSION_PATH, "op": "sample", "x": float(under[0]), "y": float(under[1])})
	var before_height := float(sampled.get("height", 0.0))
	var builds := int(_state().get("builds", 0))
	var before := device.get_texture().get_image()
	# The drag: across the middle, west to east, a sample every four pixels, one gesture.
	var gesture := await _drag(middle + Vector2(-24.0, 0.0), 0, false)
	assert_gt(gesture, 0, "the drag's gesture")
	for step in 11:
		await _drag(middle + Vector2(-20.0 + 4.0 * step, 0.0), gesture, false)
	await _drag(middle + Vector2(24.0, 0.0), gesture, true)
	assert_true(_seam.settle(), "the stroke's work run to its end")
	# The bake lands in the background; the device patches its terrain as the stroke and the landing come.
	for _frame in 600:
		var bake: Dictionary = _seam.query("terrain", {"path": MISSION_PATH, "op": "bake"})
		if not bool(bake.get("due", true)):
			break
		await get_tree().process_frame
	await _frames(20)
	sampled = _seam.query("terrain", {"path": MISSION_PATH, "op": "sample", "x": float(under[0]), "y": float(under[1])})
	assert_gt(float(sampled.get("height", 0.0)), before_height + 0.01, "the ground under the stroke raised")
	assert_eq(int(_state().get("builds", 0)), builds, "patched in place: no build of the picture")
	var after := device.get_texture().get_image()
	assert_gt(_difference(before, after, Vector2i(middle), 24), 1.0, "the picture over the stroke changed")
