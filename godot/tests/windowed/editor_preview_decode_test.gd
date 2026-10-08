extends GutTest

## The particle and definition previews draw the game's content as the game draws it (ADR 0046 "The preview
## background", its follow-up). Every engine shader writes the D3D9 gamma-domain numbers, and the game shows them
## through exactly one display decode: the FrameFx terminal its WorldEnvironment carries, which the particle
## renderer composes last behind its passes on the camera. A preview with no WorldEnvironment carries its decode on
## its scenario (DisplayDecode); the particle renderer's camera compositor replaced it there, so while particles drew
## the effect's and the definition's pictures showed their numbers encoded once more than the game's (a particle and
## a model brighter than the game draws them). Each preview's picture is read here against the game's frame: the
## game world's own render nodes (the ClearColor WorldEnvironment, FrameFx, the MissionEnvironment with no .env the
## previews light by, the EffectWorld and an ObjectModel) over the same files, seen through the preview's camera.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const CRATE := "res://../fixtures/threedi/synth/crate.3di"
const PTL := "particles/steady.ptl"
const ITEMS := "defs/items.def"
const MODEL := "models/crate.3di"
## The particle's graphic and the crate's texture: one colour each, mid tones (an encode lifts them most).
const PARTICLE_RGB := Color8(150, 90, 60)
const CRATE_RGB := Color8(90, 120, 70)
## How near a preview's pixel is to the game's, each channel.
const NEAR := 0.02
## The preview clock's ticks played before a picture is read: the steady effect's quads all alive.
const PLAY_TICKS := 63
const WAIT_MS := 30000

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	# The ImGui context past its first frames before the editor attaches to it (editor_device_test.gd says why).
	for _frame in 3:
		await get_tree().process_frame
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor preview decode %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)
	await get_tree().process_frame


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


## An 8 x 8 32-bit TGA of one opaque colour.
func _tga(rgb: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes[12] = 8
	bytes[14] = 8
	bytes[16] = 32
	bytes[17] = 0x08
	for _texel in 64:
		bytes.append_array(PackedByteArray([rgb.b8, rgb.g8, rgb.r8, 255]))
	return bytes


## Steady: opaque quads of one colour that stand at the spawn point for as long as the effect plays (no speed,
## no spread, white keys, full alpha). Speck: the same, a few centimetres wide, the item's particle slot (it keeps
## the particle renderer composing the definition's camera while the crate is read).
func _ptl() -> String:
	var text := ""
	for row: Array in [["Steady", "SteadyQuad", 2.0], ["Speck", "SpeckQuad", 0.05]]:
		text += "[effectdef]\n{\n\tid = %s;\n\tpdefs = %s;\n}\n\n" % [row[0], row[1]]
		text += "[particledef]\n{\n\tid = %s;\n\tflags = FOREVEREMIT;\n\temit_dur = 30;\n\temit_rate = 20;\n" % row[1]
		text += "\temit_burst = 1;\n\tage = 5.0;\n\tscale = 1.0;\n\tspeed = 0;\n\tspread = 0;\n"
		text += "\tcolor1 = 255, 255, 255;\n\tcolor2 = 255, 255, 255;\n\tcolor3 = 255, 255, 255;\n\tcolor4 = 255, 255, 255;\n"
		text += "\tgraphic1 = solid.tga, blend;\n\tg1_alpha = 1;\n\tg1_scale = %s;\n}\n\n" % str(row[2])
	return text


## A project holding the crate (its texture one colour), the particle file and its graphic, and an item table
## whose one record draws the crate with the Speck at its ground point.
func _project() -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor preview decode project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Preview Decode"))
	_write(dir.path_join(MODEL), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(CRATE)))
	_write(dir.path_join("models/crate.tga"), _tga(CRATE_RGB))
	_write(dir.path_join(PTL), _ptl().to_utf8_buffer())
	_write(dir.path_join("particles/solid.tga"), _tga(PARTICLE_RGB))
	_write(dir.path_join(ITEMS), TestFs.crlf("begin \"Pump station\"\nid 100500\ntype object\ngraphic crate\n"
			+ "particlefx Speck ground\nend\n").to_utf8_buffer())
	_seam.request({"kind": "rescan"})
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return dir


## The game's files for its frame: the same bytes flat in one folder, as the game's loose mount finds them.
func _game_root(project: String) -> ResourceRoot:
	var dir := OS.get_cache_dir().path_join("opennova editor preview decode game %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	for path in [MODEL, "models/crate.tga", PTL, "particles/solid.tga"]:
		_write(dir.path_join(path.get_file()), FileAccess.get_file_as_bytes(project.path_join(path)))
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	return root


func _wait_until(condition: Callable, what: String) -> bool:
	var deadline := Time.get_ticks_msec() + WAIT_MS
	while not condition.call():
		if Time.get_ticks_msec() > deadline:
			fail_test("timed out waiting for %s" % what)
			return false
		await get_tree().process_frame
	return true


func _state(path: String, kind: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "kind": kind, "limit": 1})


## The viewport of `kind` over `path` ready on its device, its clock a second in, its particles (`particles`)
## alive: then a few more frames drawn.
func _await_played(path: String, kind: String, particles: bool) -> bool:
	var played := func() -> bool:
		var state := _state(path, kind)
		var play: Dictionary = state.get("body", {}).get("play", {})
		return String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0 and int(play.get("age", play.get("tick", PLAY_TICKS))) >= PLAY_TICKS \
				and (not particles or int(play.get("particles", 0)) > 0)
	if not await _wait_until(played, "%s's viewport played" % kind):
		return false
	for _frame in 8:
		await get_tree().process_frame
	return true


## What the preview's device drew: its picture, its camera and, for a picture of a model, the model's pose.
func _preview(path: String, kind: String) -> Dictionary:
	var device: SubViewport = _app.get_viewport_device(path, kind)
	assert_not_null(device, "%s's device" % kind)
	if device == null:
		return {}
	RenderingServer.force_draw(false)
	var image := device.get_texture().get_image()
	var cameras := device.find_children("*", "Camera3D", true, false)
	assert_eq(cameras.size(), 1, "%s's one camera" % kind)
	if image == null or image.is_empty() or cameras.size() != 1:
		return {}
	var camera := cameras[0] as Camera3D
	var models := device.find_children("*", "ObjectModel", true, false)
	var model: Node3D = null
	for found: Node in models:
		if (found as ObjectModel).get_object_data() != null:
			model = found
	return {
		"image": image,
		"size": device.size,
		"camera": camera.global_transform,
		"fov": camera.fov,
		"near": camera.near,
		"far": camera.far,
		"model": model.global_transform if model != null else null,
	}


## The game's frame of the same files seen through the preview's camera: the render nodes the game world carries
## for it (game_world.tscn's ClearColor and FrameFx, the MissionEnvironment, GameWorld's EffectWorld wired to that
## environment as game_world_load wires it, an ObjectModel), the effect spawned at the origin as the preview's is,
## or the crate at the preview's pose. Drawn until the effect's quads are alive, then read.
func _game_frame(root: ResourceRoot, seen: Dictionary, effect: String, crate: bool) -> Image:
	var view := SubViewport.new()
	view.size = seen["size"]
	view.own_world_3d = true
	view.msaa_3d = Viewport.MSAA_DISABLED
	view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(view)
	var scene := Node3D.new()
	view.add_child(scene)
	var clear := WorldEnvironment.new()
	clear.name = "ClearColor"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	clear.environment = environment
	scene.add_child(clear)
	scene.add_child(FrameFx.new())
	var mission_environment := MissionEnvironment.new()
	scene.add_child(mission_environment)
	var camera := Camera3D.new()
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = seen["fov"]
	camera.near = seen["near"]
	camera.far = seen["far"]
	scene.add_child(camera)
	camera.global_transform = seen["camera"]
	camera.current = true
	if crate:
		var data := ObjectData.new()
		assert_eq(data.open_from_resource_root(root, "crate.3di", false), OK, data.get_last_error())
		var model := ObjectModel.new()
		scene.add_child(model)
		model.set_object_data(data)
		if seen["model"] != null:
			model.global_transform = seen["model"]
	var effects := EffectWorld.new()
	scene.add_child(effects)
	effects.set_environment_source(mission_environment)
	assert_gt(effects.load_from_resource_root(root), 0, "the game's effect files")
	assert_gt(effects.spawn_effect(effect, Vector3.ZERO), 0, "%s spawned" % effect)
	var start := Time.get_ticks_msec()
	for tick in PLAY_TICKS + 8:
		effects.advance_fixed_tick(1.0 / 62.5)
		effects.render_frame(start + tick * 16)
		await get_tree().process_frame
	RenderingServer.force_draw(false)
	await get_tree().process_frame
	var image := view.get_texture().get_image()
	assert_true(image != null and not image.is_empty(), "the game's frame drawn")
	return image


func _near(preview: Color, game: Color, what: String) -> void:
	gut.p("%s: the preview's %s, the game's %s" % [what, preview.to_html(false), game.to_html(false)])
	assert_true(absf(preview.r - game.r) < NEAR and absf(preview.g - game.g) < NEAR
			and absf(preview.b - game.b) < NEAR,
			"%s: the preview draws %s where the game draws %s" % [what, preview.to_html(false), game.to_html(false)])


func test_the_effect_preview_draws_a_particle_as_the_game_does() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var project := _project()
	assert_true(_seam.open_document(PTL))
	assert_true(_seam.done({"kind": "set_viewport", "path": PTL, "viewport": {"kind": "effect",
			"options": {"effect": "Steady", "grid": false}, "camera": {"distance": 6.0, "target": [0, 0, 0]}}}))
	if not await _await_played(PTL, "effect", true):
		return
	var seen := _preview(PTL, "effect")
	if seen.is_empty():
		return
	var preview: Image = seen["image"]
	var centre := Vector2i(preview.get_width() / 2, preview.get_height() / 2)
	var game := await _game_frame(_game_root(project), seen, "Steady", false)
	if game == null:
		return
	_near(preview.get_pixelv(centre), game.get_pixelv(centre), "the steady particle")


func test_the_definition_preview_draws_a_model_as_the_game_does() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	var project := _project()
	var camera := {"distance": 3.0, "target": [0, 0.5, 0], "yaw": 0.4, "pitch": 0.2}
	# The record drawn, its Speck playing at the crate's ground point (the particle renderer composing the camera).
	assert_true(_seam.open_document(ITEMS))
	assert_true(_seam.select_record(_seam.get_row_id(0)))
	assert_true(_seam.done({"kind": "set_viewport", "path": ITEMS, "viewport": {"kind": "definition",
			"options": {"grid": false}, "camera": camera, "clock": {"playing": true}}}))
	if not await _await_played(ITEMS, "definition", true):
		return
	var definition := _preview(ITEMS, "definition")
	# The same crate in the model preview, which never drew particles and kept the decode.
	assert_true(_seam.open_document(MODEL))
	assert_true(_seam.done({"kind": "set_viewport", "path": MODEL, "viewport": {"kind": "model", "camera": camera}}))
	if not await _await_played(MODEL, "model", false):
		return
	var model := _preview(MODEL, "model")
	if definition.is_empty() or model.is_empty():
		return
	var root := _game_root(project)
	for row: Array in [["definition", definition], ["model", model]]:
		var seen: Dictionary = row[1]
		var picture: Image = seen["image"]
		var centre := Vector2i(picture.get_width() / 2, picture.get_height() / 2)
		var game := await _game_frame(root, seen, "Speck", true)
		if game == null:
			return
		_near(picture.get_pixelv(centre), game.get_pixelv(centre), "the crate in the %s preview" % row[0])
