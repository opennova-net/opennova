extends GutTest

## The texture viewport's device headless through the editor's wire seam (ADR 0046 S18): a texture Files
## selects (select_file) is read from its file and drawn by its device, a canvas shader over the
## texture's levels (authoring/texture_viewport_applier) in an offscreen SubViewport, before the texture
## is opened; the GPU texture holds the texels the portable decode made (a TGA's rows taken bottom up as
## the game's reader takes them, whatever its descriptor says); the viewport's state reaches the shader
## at the next pump (the channels, the camera's scale and middle, the picture's size; the mip level of a
## DDS chain bound); opened as a document, the same device keeps drawing it; another file selected lets
## it go; shown as one of its uses, the GPU texture holds what the use's loader makes of it.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor texture %d" % Time.get_ticks_usec())
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


## A 2 x 2 uncompressed 32-bit TGA, its descriptor `descriptor`: the file's texels B, G, R, A in the
## order they are stored, texel k's red 10 + k, green 100 + k, blue 200 + k, alpha 50 + 60 k.
func _tga(descriptor: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes[12] = 2
	bytes[14] = 2
	bytes[16] = 32
	bytes[17] = descriptor
	for k in 4:
		bytes.append_array(PackedByteArray([200 + k, 100 + k, 10 + k, 50 + 60 * k]))
	return bytes


## A DXT5 DDS of `side` x `side` with its whole chain, every block the same: what each level's sides
## are is what the device binds; its texels the codec's.
func _dds(side: int) -> PackedByteArray:
	var bytes := PackedByteArray([0x44, 0x44, 0x53, 0x20])
	var levels := 0
	var s := side
	while true:
		levels += 1
		if s == 1:
			break
		s = s >> 1
	var header := PackedByteArray()
	header.resize(124)
	header.encode_u32(0, 124)
	header.encode_u32(4, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000)
	header.encode_u32(8, side)
	header.encode_u32(12, side)
	header.encode_u32(16, maxi(1, side / 4) * maxi(1, side / 4) * 16)
	header.encode_u32(24, levels)
	header.encode_u32(72, 32)
	header.encode_u32(76, 0x4)
	header.encode_u32(80, 0x35545844) # DXT5
	header.encode_u32(104, 0x1000 | 0x8 | 0x400000)
	bytes.append_array(header)
	s = side
	for _level in levels:
		var blocks := maxi(1, (s + 3) / 4) * maxi(1, (s + 3) / 4)
		for _b in blocks:
			# Alpha 255 everywhere; colour 0 pure red, colour 1 pure blue, every texel index 0.
			bytes.append_array(PackedByteArray([255, 255, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0]))
		s = maxi(1, s >> 1)
	return bytes


func _new_project() -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor texture project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "Texture Viewport"))
	return dir


func _viewport(path: String) -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": path, "kind": "texture"})


## The viewport over `path` ready on its device: a frame at a time until its device made its picture.
func _await_ready(path: String) -> Dictionary:
	var state := _viewport(path)
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and int(state.get("builds", 0)) > 0:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport(path)
	return state


## The device's canvas rect and its shader material.
func _material(path: String) -> ShaderMaterial:
	var device: SubViewport = _app.get_viewport_device(path, "texture")
	if device == null:
		return null
	var rects := device.find_children("*", "ColorRect", true, false)
	return (rects[0] as ColorRect).material as ShaderMaterial if not rects.is_empty() else null


func test_a_selected_texture_draws_through_its_device() -> void:
	if _app == null:
		return
	var root := _new_project()
	# A top-left descriptor: the game takes the rows bottom up all the same.
	_write(root.path_join("textures/brick.tga"), _tga(0x28))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.done({"kind": "select_file", "path": "textures/brick.tga"}), "select_file is served")
	var state := await _await_ready("textures/brick.tga")
	assert_eq(String(state.get("status", "")), "ready", "the selected texture shows before it is opened")
	assert_true(bool(state.get("body", {}).get("from_file", false)), "read from its file")
	assert_true(bool(state.get("body", {}).get("upside_down", false)), "a top-left TGA shows upside down")
	var material := _material("textures/brick.tga")
	assert_not_null(material, "its device draws it with the texture shader")
	if material == null:
		return
	assert_true(bool(material.get_shader_parameter("has_texture")))
	assert_eq(material.get_shader_parameter("base_size"), Vector2(2, 2))
	assert_eq(int(material.get_shader_parameter("channels")), 5, "colour over the checkerboard by default")
	# The GPU texture holds the texels as the game reads them: the file's first stored row is the bottom.
	var texture := material.get_shader_parameter("level_nearest") as Texture2D
	assert_not_null(texture)
	if texture == null:
		return
	var image := texture.get_image()
	assert_eq(image.get_size(), Vector2i(2, 2))
	assert_eq(image.get_pixel(0, 1), Color8(10, 100, 200, 50), "the file's first texel is the bottom row's first")
	assert_eq(image.get_pixel(1, 0), Color8(13, 103, 203, 230), "its last the top row's last")
	# The viewport's state reaches the shader at the next pump.
	assert_true(_seam.done({"kind": "set_viewport", "path": "textures/brick.tga",
			"viewport": {"kind": "texture", "options": {"channels": "alpha"}, "camera": {"scale": 8, "x": 1, "y": 1}}}))
	_app.pump()
	assert_eq(int(material.get_shader_parameter("channels")), 4, "the alpha as grey")
	assert_almost_eq(float(material.get_shader_parameter("scale")), 8.0, 0.001)
	assert_eq(material.get_shader_parameter("centre"), Vector2(1, 1))
	assert_true(bool(material.get_shader_parameter("nearest")), "a texel over a pixel is sampled nearest")
	# A hit names the texel under the point.
	var hit: Dictionary = _seam.query("viewport", {"op": "hit", "path": "textures/brick.tga", "kind": "texture", "x": 400, "y": 300})
	assert_eq(String(hit.get("kind", "")), "texel")
	# Opened: the same device keeps drawing it, the camera kept.
	assert_true(_seam.open_document("textures/brick.tga"))
	_app.pump()
	state = _viewport("textures/brick.tga")
	assert_false(bool(state.get("body", {}).get("from_file", true)), "read from its document once open")
	assert_eq(_material("textures/brick.tga"), material, "the same device")
	assert_almost_eq(float(material.get_shader_parameter("scale")), 8.0, 0.001)


func test_a_dds_chain_binds_the_level_shown() -> void:
	if _app == null:
		return
	var root := _new_project()
	_write(root.path_join("textures/metal.dds"), _dds(16))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.done({"kind": "select_file", "path": "textures/metal.dds"}))
	var state := await _await_ready("textures/metal.dds")
	assert_eq(String(state.get("status", "")), "ready")
	assert_eq((state.get("body", {}).get("levels", []) as Array).size(), 5, "16, 8, 4, 2 and 1")
	var material := _material("textures/metal.dds")
	assert_not_null(material)
	if material == null:
		return
	var level0 := material.get_shader_parameter("level_nearest") as Texture2D
	assert_eq(level0.get_size(), Vector2(16, 16))
	assert_eq(level0.get_image().get_pixel(3, 3), Color8(255, 0, 0, 255), "the codec's colour 0, opaque")
	assert_true(_seam.done({"kind": "set_viewport", "path": "textures/metal.dds",
			"viewport": {"kind": "texture", "options": {"level": 2}}}))
	_app.pump()
	var level2 := material.get_shader_parameter("level_nearest") as Texture2D
	assert_eq(level2.get_size(), Vector2(4, 4), "level 2 bound")
	assert_eq(material.get_shader_parameter("base_size"), Vector2(16, 16), "placed at the texture's size")
	# Another file selected: the texture's viewport and its device let go.
	_write(root.path_join("notes.txt"), "a note".to_utf8_buffer())
	_seam.request({"kind": "rescan"})
	assert_true(_seam.done({"kind": "select_file", "path": "notes.txt"}))
	_app.pump()
	assert_true(_viewport("textures/metal.dds").has("error"), "no viewport kept once another file is selected")

## S18: the texture as the game draws it for one of its uses: an item's HUD image, the HUD's alpha alone
## (an A8, white under its alpha), in the GPU texture once as_used names the use; the file again at -1.
func test_a_texture_draws_as_a_use() -> void:
	if _app == null:
		return
	var root := _new_project()
	_write(root.path_join("textures/brick.tga"), _tga(0))
	_write(root.path_join("defs/items.def"),
			TestFs.crlf("begin \"Brick\"\nid 100300\ntype building\nhud_image brick.tga\nend\n").to_utf8_buffer())
	_seam.request({"kind": "rescan"})
	assert_true(_seam.done({"kind": "select_file", "path": "textures/brick.tga"}))
	var state := await _await_ready("textures/brick.tga")
	var builds := int(state.get("builds", 0))
	assert_true(_seam.done({"kind": "set_viewport", "path": "textures/brick.tga",
			"viewport": {"kind": "texture", "options": {"as_used": 0}}}))
	for _frame in 600:
		if int(state.get("builds", 0)) > builds:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport("textures/brick.tga")
	var as_used: Dictionary = state.get("body", {}).get("as_used", {}) if state.get("body", {}).get("as_used") is Dictionary else {}
	assert_eq(String(as_used.get("transform", "")), "alpha_only", str(state.get("body", {})))
	var material := _material("textures/brick.tga")
	assert_not_null(material)
	if material == null:
		return
	var texture := material.get_shader_parameter("level_nearest") as Texture2D
	assert_eq(texture.get_image().get_pixel(0, 1), Color8(255, 255, 255, 50), "the alpha alone, white")
	builds = int(state.get("builds", 0))
	assert_true(_seam.done({"kind": "set_viewport", "path": "textures/brick.tga",
			"viewport": {"kind": "texture", "options": {"as_used": -1}}}))
	for _frame in 600:
		if int(state.get("builds", 0)) > builds:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport("textures/brick.tga")
	texture = material.get_shader_parameter("level_nearest") as Texture2D
	assert_eq(texture.get_image().get_pixel(0, 1), Color8(10, 100, 200, 50), "the file as it holds it")


## S18: a texture's whole-image edit through the wire: its rows saved bottom first (texture_operation), the
## device drawing the document's new texels once it follows; Undo puts the file's own back.
func test_a_texture_edit_redraws() -> void:
	if _app == null:
		return
	var root := _new_project()
	_write(root.path_join("textures/brick.tga"), _tga(0x28))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.open_document("textures/brick.tga"))
	var state := await _await_ready("textures/brick.tga")
	assert_true(bool(state.get("body", {}).get("upside_down", false)), "a top-left TGA shows upside down")
	var builds := int(state.get("builds", 0))
	assert_true(_seam.done({"kind": "texture_operation", "path": "textures/brick.tga", "operation": "reorder_rows"}),
			"texture_operation is served")
	for _frame in 600:
		if int(state.get("builds", 0)) > builds:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport("textures/brick.tga")
	assert_false(bool(state.get("body", {}).get("upside_down", true)), "saved bottom first, it shows the right way up")
	var material := _material("textures/brick.tga")
	assert_not_null(material)
	if material == null:
		return
	var texture := material.get_shader_parameter("level_nearest") as Texture2D
	assert_eq(texture.get_image().get_pixel(0, 0), Color8(10, 100, 200, 50), "the file's first texel now the top row's first")
	builds = int(state.get("builds", 0))
	assert_true(_seam.done({"kind": "undo", "path": "textures/brick.tga"}))
	for _frame in 600:
		if int(state.get("builds", 0)) > builds:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport("textures/brick.tga")
	assert_true(bool(state.get("body", {}).get("upside_down", false)), "Undo puts the file's own rows back")

## S18: a normal map lit: the picture the viewport made (each texel a grey of its light) in the GPU texture,
## drawn as its colour (channels 6); the light moved, made again.
func test_a_normal_map_lit() -> void:
	if _app == null:
		return
	var root := _new_project()
	_write(root.path_join("textures/brick.tga"), _tga(0))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.done({"kind": "select_file", "path": "textures/brick.tga"}), "select_file is served")
	var first := await _await_ready("textures/brick.tga")
	assert_true(_seam.done({"kind": "set_viewport", "path": "textures/brick.tga",
			"viewport": {"kind": "texture", "options": {"channels": "normals", "light": 0}}}))
	# The picture made again for the lit normals: a build past the first.
	var state := _viewport("textures/brick.tga")
	for _frame in 60:
		_app.pump()
		await get_tree().process_frame
		state = _viewport("textures/brick.tga")
		if int(state.get("builds", 0)) > int(first.get("builds", 0)):
			break
	var builds := int(state.get("builds", 0))
	assert_gt(builds, int(first.get("builds", 0)), "the lit picture made")
	var material := _material("textures/brick.tga")
	assert_not_null(material, str(state))
	if material == null:
		return
	assert_eq(int(material.get_shader_parameter("channels")), 6, "the lit picture drawn as its colour")
	var texture := material.get_shader_parameter("level_nearest") as Texture2D
	var lit := texture.get_image().get_pixel(0, 0)
	assert_almost_eq(lit.r, lit.g, 0.001, "a grey")
	assert_almost_eq(lit.g, lit.b, 0.001, "a grey")
	assert_eq(lit.a, 1.0, "opaque")
	assert_true(_seam.done({"kind": "set_viewport", "path": "textures/brick.tga",
			"viewport": {"kind": "texture", "options": {"light": 180}}}))
	for _frame in 60:
		_app.pump()
		await get_tree().process_frame
		if int(_viewport("textures/brick.tga").get("builds", 0)) > builds:
			break
	assert_gt(int(_viewport("textures/brick.tga").get("builds", 0)), builds, "the light moved, the picture made again")


## S18: a texture replaced by an image through the wire: the image an import source in art/ under the
## texture's stem (brick.tga's art/brick.png, whatever the image is called), the texture its import's output
## under its own name, the file it replaced set aside.
func test_a_texture_replaced_by_an_image() -> void:
	if _app == null:
		return
	var root := _new_project()
	_write(root.path_join("textures/brick.tga"), _tga(0))
	var outside := OS.get_cache_dir().path_join("opennova texture image %d" % Time.get_ticks_usec())
	_dirs.append(outside)
	assert_eq(DirAccess.make_dir_recursive_absolute(outside), OK)
	var image := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	image.fill(Color8(1, 2, 3, 255))
	var png := outside.path_join("mine.png")
	assert_eq(image.save_png(png), OK)
	_seam.request({"kind": "rescan"})
	assert_true(_seam.settle())
	assert_true(_seam.done({"kind": "replace_texture", "path": "textures/brick.tga", "paths": [png]}),
			"replace_texture is served")
	assert_true(_seam.settle())
	var made := ""
	for file in _seam.every("files", "files", {"limit": 200}):
		if String(file.get("name", "")) == "brick.tga":
			made = String(file.get("imported_from", ""))
	assert_eq(made, "art/brick.png", "the texture is the image's import output now")
	assert_false(FileAccess.file_exists(root.path_join("textures/brick.tga")), "the file it replaced is set aside")


## S18: the OS's drop held where its cursor let go: a screen point taken into the window's viewport (its
## client area's position off, its stretch undone), never the viewport's last mouse event, which a drag from
## another program over the window never moves.
func test_an_os_drop_is_held_where_the_cursor_let_go() -> void:
	if _app == null:
		return
	var window: Window = _app.get_window()
	var inside := Vector2(300, 200)
	var screen := Vector2(window.position) + window.get_final_transform() * inside
	_app.drop_files_at_screen(PackedStringArray(["C:/art/new.png"]), screen)
	var held: Vector2 = _app.get_last_drop_at()
	assert_almost_eq(held.x, inside.x, 0.01, "the drop's own point, across")
	assert_almost_eq(held.y, inside.y, 0.01, "and down")
	assert_ne(window.get_mouse_position(), inside, "not the viewport's last mouse position")


## S18: a texture opened in its program through the wire: edit_externally's open_externally event taken by
## the Shell at the next pump (kept, not opened, with open_externally off), the source a PNG of the TGA's
## texels (art/brick.png); what the program saves comes
## back of the Shell's own: the window gaining the focus checks at once (a file written just now waits,
## never read half-written), then once a second while it has the focus, the texture made from it again.
func test_a_texture_edited_in_its_program() -> void:
	if _app == null:
		return
	_app.set("open_externally", false)
	var root := _new_project()
	_write(root.path_join("textures/brick.tga"), _tga(0))
	_seam.request({"kind": "rescan"})
	assert_true(_seam.settle())
	assert_true(_seam.done({"kind": "edit_externally", "path": "textures/brick.tga"}), "edit_externally is served")
	assert_true(_seam.settle())
	_app.pump()
	var source := root.path_join("art/brick.png")
	assert_eq(String(_app.get_last_external_open()).replace("\\", "/"), source.replace("\\", "/"),
			"the Shell takes the source it names")
	var output := ""
	for file in _seam.every("files", "files", {"limit": 200}):
		if String(file.get("name", "")) == "brick.tga":
			output = String(file.get("path", ""))
	assert_true(_seam.open_document(output), "the import's output opens: %s" % output)
	var state := await _await_ready(output)
	var material := _material(output)
	assert_not_null(material, str(state))
	if material == null:
		return
	var builds := int(state.get("builds", 0))
	# The program saves the source; the window gains the focus at once: written just now, it waits.
	var saved := _tga(0)
	saved[18] = 0
	saved[19] = 0
	saved[20] = 255
	# Saved as the PNG it is, the TGA's texels where an image program puts them (its first stored row the bottom).
	var saved_image := Image.new()
	assert_eq(saved_image.load_tga_from_buffer(saved), OK)
	assert_eq(saved_image.save_png(source), OK)
	_app.notification(NOTIFICATION_APPLICATION_FOCUS_IN)
	assert_true(_seam.settle())
	_app.pump()
	assert_eq(int(_viewport(output).get("builds", 0)), builds, "a file written just now is not read yet")
	# Settled, the once-a-second check while the window has the focus imports it again.
	var deadline := Time.get_ticks_msec() + 10000
	while Time.get_ticks_msec() < deadline and int(_viewport(output).get("builds", 0)) == builds:
		_app.pump()
		await get_tree().create_timer(0.1).timeout
	state = await _await_ready(output)
	var texture := material.get_shader_parameter("level_nearest") as Texture2D
	assert_eq(texture.get_image().get_pixel(0, 1), Color8(255, 0, 0, 50), "the texel the program saved")
	_app.notification(NOTIFICATION_APPLICATION_FOCUS_OUT)