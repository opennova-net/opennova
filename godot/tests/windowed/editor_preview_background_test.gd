extends GutTest

## The editor's Preview background (editor/session/preview_background.h) as each preview's device renders it, in
## a window, the Preview window's canvas drawing the device each frame: the model's (the picture a clip and an
## animation table play on too), the effect's and the definition's 3D pictures, the texture's and the HUD's 2D
## ones. Read off the rendered picture's edges: Grey's mid grey at the top falling to a darker one at the bottom,
## Light's pale greys, Checker's two greys 8 pixels a square from the top left, and Dark each picture's own as
## before the preference (a 3D picture's near-black clear, the texture's 0.16 grey, the HUD's mid grey). A 3D
## picture shows its numbers as they are through its one display decode, so the colours read as the 2D ones do.
## Each is set as the editor sets it (set_preview_background) and drawn at the device's next frames, live.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const ARMORY := "res://../fixtures/threedi/synth/armory.3di"
const CRATE := "res://../fixtures/threedi/synth/crate.3di"
const PTL := "particles/puff.ptl"
const ITEMS := "defs/items.def"
const LAYOUT := "defs/hudpos.def"
const TEXTURE := "textures/dark.tga"
## How near a rendered edge pixel is to the colour it should be, each channel.
const NEAR := 0.025

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor preview background %d" % Time.get_ticks_usec())
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


## A `width` x `height` 32-bit TGA of one colour (a dark one: what used to vanish on the old background).
func _tga(width: int, height: int, rgb := Color(0.1, 0.12, 0.08)) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes[12] = width
	bytes[14] = height
	bytes[16] = 32
	bytes[17] = 0x08
	for _texel in width * height:
		bytes.append_array(PackedByteArray([rgb.b8, rgb.g8, rgb.r8, 255]))
	return bytes


func _ptl() -> String:
	return ("[effectdef]\n{\n\tid = Puff;\n\tpdefs = PuffDot;\n}\n\n[particledef]\n{\n\tid = PuffDot;\n\temit_dur = 1.0;\n"
			+ "\temit_rate = 40;\n\temit_burst = 1;\n\tage = 1.0;\n\tscale = 1.0;\n\tspeed = 1.5;\n\tspread = 40;\n"
			+ "\tgraphic1 = particle_dot.tga, blend;\n\tg1_alpha = 1;\n\tg1_scale = 1;\n}\n")


## A project holding one of each picture's files: a model, a particle file, an item table drawing the model, a HUD
## layout and its textures, a dark texture.
func _project() -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor preview background project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Preview Background"))
	_write(dir.path_join("models/armory.3di"), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(ARMORY)))
	_write(dir.path_join("models/crate.3di"), FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(CRATE)))
	_write(dir.path_join(PTL), _ptl().to_utf8_buffer())
	_write(dir.path_join("particles/particle_dot.tga"),
			FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/cbin/particle_dot.tga")))
	_write(dir.path_join(ITEMS), TestFs.crlf("begin \"Pump station\"\nid 100500\ntype object\ngraphic crate\nend\n")
			.to_utf8_buffer())
	_write(dir.path_join(LAYOUT), TestFs.crlf("StaticFrame frame.tga 6,586\nHUDHEALTH 25,741,177,751\n").to_utf8_buffer())
	_write(dir.path_join("textures/frame.tga"), _tga(8, 8))
	# Narrow and tall: fitted to the picture's height, it leaves the picture's sides to the background.
	_write(dir.path_join(TEXTURE), _tga(2, 8))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.settle(), "a Rescan steps across pumps (S13 A3)")
	return dir


## The viewport over `path` of `kind` ready on its device, then drawn a few frames.
func _await_drawn(path: String, kind: String) -> bool:
	var ready := false
	for _frame in 600:
		var state: Dictionary = _seam.query("viewport", {"op": "state", "path": path, "kind": kind, "limit": 1})
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			ready = true
			break
		await get_tree().process_frame
	assert_true(ready, "%s's viewport ready" % kind)
	for _frame in 6:
		await get_tree().process_frame
	return ready


## The device's last rendered picture, a few frames after the background was set.
func _picture(path: String, kind: String) -> Image:
	for _frame in 6:
		await get_tree().process_frame
	var device: SubViewport = _app.get_viewport_device(path, kind)
	assert_not_null(device, "%s's device" % kind)
	if device == null:
		return null
	var image := device.get_texture().get_image()
	assert_true(image != null and not image.is_empty() and image.get_width() > 24 and image.get_height() > 24,
			"%s's picture" % kind)
	return image if image != null and not image.is_empty() and image.get_width() > 24 else null


func _near(pixel: Color, rgb: int, what: String) -> void:
	var want := Color.hex((rgb << 8) | 0xFF)
	assert_true(absf(pixel.r - want.r) < NEAR and absf(pixel.g - want.g) < NEAR and absf(pixel.b - want.b) < NEAR,
			"%s: %s, not #%06x" % [what, pixel.to_html(false), rgb])


## Each background set and read off the picture's left edge, `x` pixels in (a column the picture's content leaves
## to its background); `own` checks Dark's.
func _check_each(path: String, kind: String, x: int, own: Callable) -> void:
	for background: String in ["grey", "light", "checker", "dark"]:
		assert_true(_seam.done({"kind": "set_preview_background", "preview_background": background}), background)
		var image := await _picture(path, kind)
		if image == null:
			return
		var low := image.get_height() - 3
		var what := "%s on %s" % [kind, background]
		match background:
			"grey":
				_near(image.get_pixel(x, 2), 0x5A5A5A, what + " at the top")
				_near(image.get_pixel(x, low), 0x3C3C3C, what + " at the bottom")
			"light":
				_near(image.get_pixel(x, 2), 0xD2D2D2, what + " at the top")
				_near(image.get_pixel(x, low), 0xB4B4B4, what + " at the bottom")
			"checker":
				# The square at the top left and the one beside it (squares of 8 from the picture's corner).
				_near(image.get_pixel(x % 8, 2), 0x9E9E9E, what + "'s first square")
				_near(image.get_pixel(8 + x % 8, 2), 0x666666, what + "'s second square")
			"dark":
				own.call(image.get_pixel(x, 2), what)


func _dark_3d(pixel: Color, what: String) -> void:
	assert_true(pixel.r < 0.15 and pixel.g < 0.15 and pixel.b < 0.15, "%s: the old near-black clear, %s" % [what, pixel.to_html(false)])


func test_each_preview_draws_each_background() -> void:
	if _app == null:
		return
	assert_true(_app.is_available(), "a window: the editor's ImGui pass attached, its canvases drawing")
	_project()
	# The model's picture (a clip's and an animation table's are the same device).
	assert_true(_seam.open_document("models/armory.3di"))
	if await _await_drawn("models/armory.3di", "model"):
		await _check_each("models/armory.3di", "model", 2, _dark_3d)
	# The effect's, its grid hidden (its lines cross the left edge).
	assert_true(_seam.open_document(PTL))
	if await _await_drawn(PTL, "effect"):
		assert_true(_seam.done({"kind": "set_viewport", "path": PTL, "viewport": {"kind": "effect", "options": {"grid": false}}}))
		await _check_each(PTL, "effect", 2, _dark_3d)
	# The definition's, the table's record drawn, its grid hidden.
	assert_true(_seam.open_document(ITEMS))
	assert_true(_seam.select_record(_seam.get_row_id(0)))
	if await _await_drawn(ITEMS, "definition"):
		assert_true(_seam.done({"kind": "set_viewport", "path": ITEMS, "viewport": {"kind": "definition", "options": {"grid": false}}}))
		await _check_each(ITEMS, "definition", 2, _dark_3d)
	# The texture's, around the image: its own 0.16 grey on Dark.
	assert_true(_seam.done({"kind": "select_file", "path": TEXTURE}))
	if await _await_drawn(TEXTURE, "texture"):
		await _check_each(TEXTURE, "texture", 2, func(pixel: Color, what: String) -> void: _near(pixel, 0x292929, what))
	# The HUD's, its own mid grey on Dark (0.27, 0.29, 0.31).
	assert_true(_seam.open_document(LAYOUT))
	if await _await_drawn(LAYOUT, "hud"):
		await _check_each(LAYOUT, "hud", 400, func(pixel: Color, what: String) -> void: _near(pixel, 0x454A4F, what))
