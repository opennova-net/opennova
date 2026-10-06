extends GutTest

## The definition viewport's device headless through the editor's wire seam (ADR 0046 DI-21): a record of an
## items.def selected draws its item in the Preview as the game draws it: its graphic as the runtime's
## ObjectModel in an offscreen SubViewport (authoring/preview_model, built over frames), its particle slot's
## effect at its user point drawn by the game's particle renderer (authoring/preview_effects, DI-14's helper), its
## death as DI-10 plans it: destroyed past the husk swap, the husk drawn in the graphic's place with its pieces'
## sections hidden and the destroy fade on its registers, the death's effect spawned with it.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const ITEMS := "defs/items.def"
const ARMORY := "res://../fixtures/threedi/synth/armory.3di"
const CRATE := "res://../fixtures/threedi/synth/crate.3di"

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor definition %d" % Time.get_ticks_usec())
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


## Puff: a burst of dots of the fixture's graphic that emits for long enough to be seen.
func _ptl() -> String:
	var text := "[effectdef]\n{\n\tid = Puff;\n\tpdefs = PuffDot;\n}\n\n"
	text += "[particledef]\n{\n\tid = PuffDot;\n\temit_dur = 30;\n\temit_rate = 40;\n\temit_burst = 1;\n"
	text += "\tage = 1.0;\n\tscale = 1.0;\n\tspeed = 1.5;\n\tspread = 40;\n\tgraphic1 = particle_dot.tga, blend;\n"
	text += "\tg1_alpha = 1;\n\tg1_scale = 1;\n}\n\n"
	return text


func _new_project() -> void:
	var dir := OS.get_cache_dir().path_join("opennova editor definition project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Definition Viewport"))
	_write(dir.path_join("models/crate.3di"), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(CRATE)))
	_write(dir.path_join("models/armory.3di"), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(ARMORY)))
	_write(dir.path_join("particles/puff.ptl"), _ptl().to_utf8_buffer())
	var dot := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/cbin/particle_dot.tga"))
	assert_gt(dot.size(), 18, "the fixture's particle graphic reads")
	_write(dir.path_join("particles/particle_dot.tga"), dot)
	_write(dir.path_join(ITEMS), TestFs.crlf("begin \"Pump station\"\nid 100500\ntype object\ngraphic crate\n"
			+ "ai_function gnrc\nhusk armory\nhusk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M 04_CHUNK_S\n"
			+ "destroy_timing 0.5 1.0 0.25\nparticlefx Puff ground\nparticledeath Puff\nend\n").to_utf8_buffer())
	_seam.request({"kind": "rescan"})
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")


func _state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": ITEMS, "kind": "definition", "limit": 50})


func _change(change: Dictionary) -> bool:
	var request := {"kind": "set_viewport", "path": ITEMS, "viewport": change.merged({"kind": "definition"})}
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return bool(answer.get("outcome", {}).get("done", false))


## The viewport ready on its device, a build newer than `builds` ended: a frame at a time.
func _await_built(builds := 0) -> Dictionary:
	var state := _state()
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > builds:
			break
		_app.pump()
		await get_tree().process_frame
		state = _state()
	return state


func _device_node(type: String) -> Node:
	var device: SubViewport = _app.get_viewport_device(ITEMS, "definition")
	if device == null:
		return null
	var found := device.find_children("*", type, true, false)
	return found[0] if not found.is_empty() else null


func test_a_record_draws_its_item_its_effects_and_its_death() -> void:
	if _app == null:
		return
	_new_project()
	assert_true(_seam.open_document(ITEMS), "the item table opens")
	assert_true(_seam.select_record(_seam.get_row_id(0)), "its first record selected")
	assert_true(_change({"clock": {"playing": true, "ticks": 0}}))
	var state := await _await_built()
	assert_eq(String(state.get("status", "")), "ready", str(state.get("message", "")))
	var body: Dictionary = state.get("body", {})
	assert_eq(String(body.get("record", {}).get("name", "")), "Pump station")
	assert_eq(String(body.get("draws", {}).get("file", "")), "models/crate.3di", str(body.get("draws", {})))
	# The graphic drawn by the runtime's ObjectModel, the particle slot through the game's particle renderer.
	var model := _device_node("ObjectModel") as ObjectModel
	assert_not_null(model, "its device draws an ObjectModel")
	var renderer := _device_node("ParticleRenderer") as ParticleRenderer
	assert_not_null(renderer, "and the game's particle renderer")
	if model == null or renderer == null:
		return
	for _frame in 600:
		if model.get_object_data() != null:
			break
		_app.pump()
		await get_tree().process_frame
	assert_not_null(model.get_object_data(), "the crate built")
	assert_eq(model.get_object_data().get_light_count(), 0, "the crate, intact")
	for _frame in 20:
		_app.pump()
		await get_tree().process_frame
	body = _state().get("body", {})
	var effects: Array = body.get("effects", [])
	assert_eq(effects.size(), 1, str(effects))
	if effects.size() == 1:
		assert_eq(String(effects[0].get("source", "")), "particle_slot")
		assert_eq(String(effects[0].get("point", "")), "ground", "the slot at the crate's ground point")
	assert_gt(int(body.get("play", {}).get("particles", 0)), 0, "the slot's effect plays")
	assert_gt(renderer.get_rendered_quad_count(), 0, "the renderer drew it")
	assert_eq((body.get("missing_graphics", []) as Array).size(), 0, "its graphic read from the project's files")

	# Destroying, the clock past the swap and into the fade: the husk in the crate's place.
	var builds := int(_state().get("builds", 0))
	assert_true(_change({"options": {"state": "destroying"}, "clock": {"ticks": 93, "playing": false}}))
	state = await _await_built(builds)
	for _frame in 600:
		if model.get_object_data() != null and model.get_object_data().get_light_count() == 2:
			break
		_app.pump()
		await get_tree().process_frame
	body = _state().get("body", {})
	assert_eq(String(body.get("draws", {}).get("field", "")), "husk", str(body.get("draws", {})))
	assert_eq(model.get_object_data().get_light_count(), 2, "the armory, the husk, drawn in the crate's place")
	assert_false((model.get_node("Robj_2") as Node3D).visible, "a CHUNK_M flown off")
	assert_false((model.get_node("Robj_3") as Node3D).visible, "a CHUNK_S flown off")
	var values: Dictionary = model.get_ctrl_values()
	assert_eq(int(values.get("OBJECT_DESTROY01", 0)), 65536, str(values))
	# The death's effect at the item beside the slot's.
	var sources := []
	for effect: Variant in body.get("effects", []):
		sources.append(String(effect.get("source", "")))
	assert_true(sources.has("death") or sources.has("dead"), str(sources))
	# The grid hidden at the next pump.
	var device: SubViewport = _app.get_viewport_device(ITEMS, "definition")
	var grids := device.find_children("Grid", "MeshInstance3D", true, false)
	assert_eq(grids.size(), 1, "the device's grid")
	if grids.is_empty():
		return
	var grid := grids[0] as MeshInstance3D
	assert_true(_change({"options": {"grid": false}}))
	assert_false(grid.visible, "the grid hidden")
