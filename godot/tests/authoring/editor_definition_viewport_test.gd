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


## DI-22: a weapon fires in the definition preview as the game fires it. The gun (the skinned fixture with a muzzle
## and a shell point) and the character's arms draw on the gun's rig in the device, seen from the eye through the
## weapon's renderfov; a held trigger runs the game's local player in the range: the gun posed by the fire clip,
## the rounds' tracers drawn as the game's ribbons, their stops on the target leaving scars the game's ScarPresenter
## draws, the shot heard by the Shell.
const SKINNED := "res://../fixtures/threedi/o3d/skinned.o3d"
const GUN_CLIPS := """o3a 1
adm GUN.adm
row anim_reset "rest"
row anim_wpn_idle "rest"
row anim_wpn_fire "fire"
clip rest
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
clip fire
fps 30
flags 0x0
frames 2
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0.3826834 0.9238795
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
"""


func _wave_bytes(seconds: float) -> PackedByteArray:
	var samples := int(22050 * seconds)
	var data := PackedByteArray()
	data.resize(samples * 2)
	for i in samples:
		data.encode_s16(i * 2, int(sin(i * 0.1) * 8000.0))
	var head := PackedByteArray()
	head.resize(44)
	head.encode_u32(0, 0x46464952) # RIFF
	head.encode_u32(4, 36 + data.size())
	head.encode_u32(8, 0x45564157) # WAVE
	head.encode_u32(12, 0x20746d66) # "fmt "
	head.encode_u32(16, 16)
	head.encode_u16(20, 1) # PCM
	head.encode_u16(22, 1) # mono
	head.encode_u32(24, 22050)
	head.encode_u32(28, 44100)
	head.encode_u16(32, 2)
	head.encode_u16(34, 16)
	head.encode_u32(36, 0x61746164) # data
	head.encode_u32(40, data.size())
	head.append_array(data)
	return head


func _weapon_state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": "weapon.def", "kind": "definition", "limit": 50})


func _weapon_change(change: Dictionary) -> bool:
	var request := {"kind": "set_viewport", "path": "weapon.def", "viewport": change.merged({"kind": "definition"})}
	var answer: Dictionary = _seam.request(request)
	_app.pump()
	return bool(answer.get("outcome", {}).get("done", false))


func test_a_weapon_fires_in_first_person() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor definition weapon %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	var root := dir.path_join("project")
	assert_true(_seam.new_project(root, "Weapon Fire"))
	var source := dir.path_join("source")
	var gun := FileAccess.get_file_as_string(ProjectSettings.globalize_path(SKINNED))
	gun += "userpoint \"muzzle\" 0 1 0.5 0 1 0 1 83\nuserpoint \"shell\" 0.25 0 0 1 0 0 0 83\n"
	_write(source.path_join("gun.o3d"), gun.to_utf8_buffer())
	_write(source.path_join("arms.o3d"), gun.to_utf8_buffer())
	_write(source.path_join("gun.o3a"), GUN_CLIPS.to_utf8_buffer())
	_app.request_json(JSON.stringify({"kind": "import_files", "imports": [
		{"path": source.path_join("gun.o3d")}, {"path": source.path_join("arms.o3d")},
		{"path": source.path_join("gun.o3a")}]}))
	assert_true(_seam.settle(), "the import steps across pumps")
	_write(root.path_join("weapon.def"), TestFs.crlf("weapon \"WPN_TEST\"\n\tclipsize 30\n\tstartrounds 90\n"
			+ "\tanimadm gun\n\tgfx1 gun\n\tgfx3 gun\n\tround_type AMMO_TEST\n\tflags auto\n"
			+ "\tpos 25 -5 -145 0 0 0\n\taction \"fire\"\n\t\tanim anim_wpn_fire\n\t\tsoundsetend GS_TEST\n"
			+ "\t\tdelayend 4\n\t\tparticle Puff\n\t\tparticleuserpoint muzzle\n\t\tfunction wpn_std_fire\n\tend\n"
			+ "\taction \"recoil\"\n\t\tparticle Puff\n\t\tparticleuserpoint shell\n\t\tfunction wpn_std_recoil\n"
			+ "\tend\nend\n").to_utf8_buffer())
	_write(root.path_join("ammo.def"), TestFs.crlf("ammo AT_NULL\nend\nammo AMMO_TEST\n\tvelocity 600\n\tmax_age 3\n"
			+ "\tweight_in_grains 62\n\ttracerrate 1\n\ttracer_type 1 2\n\tscar_type 1\n\teffects_table\n"
			+ "\t\tobj Puff GS_TEST 15\n\t\tdirt Puff GS_TEST 15\n\tend\nend\n").to_utf8_buffer())
	_write(root.path_join("Avatars.def"), TestFs.crlf("define head H1\n{\n\tname AV_H\n\tgraphic gun.3di\n}\n"
			+ "define body B1\n{\n\tname AV_B\n\tgraphic gun.3di\n}\n"
			+ "define arms A1\n{\n\tname AV_A\n\tgraphic arms.3di\n\tcamo 1 2 3\n}\n"
			+ "nationality 0 AV_GOOD\n{\n\talignment good\n\tdivision 0 AV_DIV\n\t{\n\t\tcombo 1 H1 B1 A1\n\t}\n}\n")
			.to_utf8_buffer())
	_write(root.path_join("particles/puff.ptl"), _ptl().to_utf8_buffer())
	_write(root.path_join("particles/particle_dot.tga"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/cbin/particle_dot.tga")))
	_write(root.path_join("sounds/gs_test.wav"), _wave_bytes(0.2))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	# The bank made in the editor: the shot, one layer playing its wave.
	assert_true(_seam.done({"kind": "create_file", "path": "game.lwf"}))
	assert_true(_seam.done({"kind": "edit_record", "path": "game.lwf", "open_first": true, "edits": [
		{"op": "add", "kind": "wave", "as": "w"}, {"op": "set", "id": "w", "field": "name", "value": "GS_TEST"},
		{"op": "set", "id": "w", "field": "file", "value": "gs_test.wav"},
		{"op": "add", "kind": "set", "as": "s"}, {"op": "set", "id": "s", "field": "name", "value": "GS_TEST"},
		{"op": "add", "kind": "layer", "parent": "s", "as": "l"},
		{"op": "add", "kind": "member", "parent": "l", "field": "wave", "value": "GS_TEST"}]}))
	assert_true(_seam.done({"kind": "save_all"}))
	assert_true(_seam.open_document("weapon.def"), "the weapon table opens")
	assert_true(_seam.select_record(_seam.get_row_id(0)), "its record selected")
	assert_true(_weapon_change({"options": {"weapon": "first"}, "clock": {"playing": false, "ticks": 0}}))
	var state := _weapon_state()
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		_app.pump()
		await get_tree().process_frame
		state = _weapon_state()
	assert_eq(String(state.get("status", "")), "ready", str(state.get("message", "")))
	var weapon: Dictionary = state.get("body", {}).get("weapon", {})
	assert_eq(String(weapon.get("view", "")), "first", str(weapon))
	assert_eq(String(weapon.get("ammo", "")), "AMMO_TEST", str(weapon))
	var device: SubViewport = _app.get_viewport_device("weapon.def", "definition")
	assert_not_null(device)
	if device == null:
		return
	# The gun and the arms on the gun's rig, built over the frames.
	var gun_model: ObjectModel = null
	var arms: ObjectModel = null
	for _frame in 600:
		gun_model = null
		arms = null
		for found: Variant in device.find_children("*", "ObjectModel", true, false):
			var model := found as ObjectModel
			if model.get_object_data() == null:
				continue
			if model.get_avatar_part() == ObjectModel.AVATAR_PART_ARMS:
				arms = model
			else:
				gun_model = model
		if gun_model != null and arms != null:
			break
		_app.pump()
		await get_tree().process_frame
	assert_not_null(gun_model, "the gun draws")
	assert_not_null(arms, "the arms draw beside it")
	if gun_model == null or arms == null:
		return
	assert_true(gun_model.has_skeleton() and arms.has_skeleton(), "both ride the gun's rig")
	assert_eq(int(arms.get_ctrl_values().get("TEX_CAMO1", -1)), 1, str(arms.get_ctrl_values()))
	assert_eq(int(gun_model.get_ctrl_values().get("TEX_TEAM", -1)), 1, str(gun_model.get_ctrl_values()))
	# The eye: the device's camera through the weapon's renderfov.
	var camera := device.find_children("*", "Camera3D", true, false)[0] as Camera3D
	assert_almost_eq(camera.fov, 80.0, 1e-4)
	# The trigger held: the rounds fly, their tracers drawn, their stops scarring the target.
	var tracers := device.find_children("Tracers", "MeshInstance3D", true, false)[0] as MeshInstance3D
	var target := device.find_children("Target", "MeshInstance3D", true, false)[0] as MeshInstance3D
	var scars := device.find_children("Scars", "ScarPresenter", true, false)[0] as ScarPresenter
	var started_before: int = _app.get_clip_voices_started()
	assert_true(_weapon_change({"gestures": [{"tick": 0, "gesture": "hold"}],
			"clock": {"playing": true, "ticks": 0}}))
	var ribbons := 0
	var fire_clip := false
	for _frame in 900:
		ribbons = max(ribbons, (tracers.mesh as ArrayMesh).get_surface_count())
		fire_clip = fire_clip or gun_model.get_active_body_clip() == "anim_wpn_fire"
		weapon = _weapon_state().get("body", {}).get("weapon", {})
		if int(weapon.get("scars", 0)) >= 2 and ribbons > 0 and fire_clip \
				and _app.get_clip_voices_started() > started_before:
			break
		_app.pump()
		await get_tree().process_frame
	assert_gt(int(weapon.get("shots", 0)), 1, "the held trigger fires on: %s" % str(weapon.get("state", {})))
	assert_gt(ribbons, 0, "the tracers drawn as the game's ribbons")
	assert_true(fire_clip, "the gun posed by the fire clip")
	assert_true(target.visible, "the target stands")
	assert_gt(int(weapon.get("scars", 0)), 0, "the stops scar the target")
	assert_gt(scars.get_stats_record().world_surfaces, 0, "the game's ScarPresenter draws them")
	assert_gt(_app.get_clip_voices_started(), started_before, "the Shell started the shot's wave")
	var sources := {}
	for effect: Variant in _weapon_state().get("body", {}).get("effects", []):
		sources[String(effect.get("source", ""))] = true
	assert_true(sources.has("begin") and sources.has("direct") and sources.has("impact"), str(sources.keys()))
	assert_true(_weapon_change({"clock": {"playing": false}, "gestures": []}))


func _ammo_state() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": "ammo.def", "kind": "definition", "limit": 50})


## DI-23: an ammo record whose rounds draw no model of their own stands all the same: its impact rows as a board in
## the body, and a row played (`impact`) fires one round alone at a face of it, the device drawing the range's
## target, the impact's effect through the game's particle renderer and the scar through its ScarPresenter.
func test_an_ammo_plays_its_impact_rows() -> void:
	if _app == null:
		return
	var dir := OS.get_cache_dir().path_join("opennova editor definition ammo %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Ammo Board"))
	_write(dir.path_join("ammo.def"), TestFs.crlf("ammo AT_NULL\n\teffects_table\n\t\tmove none none 0\n"
			+ "\t\tplayer none none 0\n\t\tzip none none 0\n\t\tobj Puff none 15\n\t\tdirt Puff none 15\n\tend\nend\n"
			+ "ammo AMMO_T\n\tvelocity 600\n\tmax_age 3\n\tweight_in_grains 62\n\tscar_type 1\n\teffects_table\n"
			+ "\t\tmetal Puff none 15\n\tend\nend\n").to_utf8_buffer())
	_write(dir.path_join("particles/puff.ptl"), _ptl().to_utf8_buffer())
	_write(dir.path_join("particles/particle_dot.tga"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/cbin/particle_dot.tga")))
	_app.request_json(JSON.stringify({"kind": "rescan"}))
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	assert_true(_seam.open_document("ammo.def"), "the ammo table opens")
	assert_true(_seam.select_record(_seam.get_row_id(1)), "AMMO_T selected")
	var state := _ammo_state()
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		_app.pump()
		await get_tree().process_frame
		state = _ammo_state()
	assert_eq(String(state.get("status", "")), "ready", str(state.get("message", "")))
	var impacts: Dictionary = state.get("body", {}).get("impacts", {})
	assert_eq(String(impacts.get("ammo", "")), "AMMO_T", str(impacts).left(300))
	var surfaces: Array = impacts.get("surfaces", [])
	assert_eq(surfaces.size(), 20)
	if surfaces.size() != 20:
		return
	# Dirt (class 1): AMMO_T authors none, so the game plays AT_NULL's bank at place 5, its dirt row.
	assert_eq(String(surfaces[1].get("from", "")), "bank")
	assert_eq(int(surfaces[1].get("bank_tag", -1)), 5)
	assert_eq(String(surfaces[1].get("effect", "")), "Puff")
	assert_eq(String(surfaces[14].get("from", "")), "own")
	# The metal row played: one round alone at a metal face.
	var request := {"kind": "set_viewport", "path": "ammo.def", "viewport": {"kind": "definition", "impact": "metal"}}
	assert_true(bool(_seam.request(request).get("outcome", {}).get("done", false)))
	var device: SubViewport = _app.get_viewport_device("ammo.def", "definition")
	assert_not_null(device)
	if device == null:
		return
	var target := device.find_children("Target", "MeshInstance3D", true, false)[0] as MeshInstance3D
	var scars := device.find_children("Scars", "ScarPresenter", true, false)[0] as ScarPresenter
	var weapon := {}
	for _frame in 600:
		weapon = _ammo_state().get("body", {}).get("weapon", {})
		if int(weapon.get("scars", 0)) > 0 and scars.get_stats_record().world_surfaces > 0:
			break
		_app.pump()
		await get_tree().process_frame
	assert_true(target.visible, "the target stands")
	assert_eq(String(weapon.get("range", {}).get("target", {}).get("surface", "")), "metal")
	assert_gt(int(weapon.get("scars", 0)), 0, "the round scars the face: %s" % str(weapon.get("events", [])).left(400))
	assert_gt(scars.get_stats_record().world_surfaces, 0, "the game's ScarPresenter draws it")
	var impact_spawns := 0
	for effect: Variant in _ammo_state().get("body", {}).get("effects", []):
		if String(effect.get("source", "")) == "impact":
			impact_spawns += 1
	assert_gt(impact_spawns, 0, "the impact's effect spawned")
