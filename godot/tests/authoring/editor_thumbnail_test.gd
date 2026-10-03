extends GutTest

## The texture thumbnails' device headless (ADR 0046 S18): a texture file's thumbnail, made by the
## session's portable cache (editor/preview/texture_thumbnails), uploaded as a GPU texture holding the
## texels the cache made (a TGA's rows as the game's reader takes them, bottom up; what the use's loader
## makes of them applied: the HUD's alpha alone), one texture a picture however often it is asked for;
## a file that is no texture has none; the texture_thumbnail query answers the same picture as a PNG.

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
	var settings_dir := OS.get_cache_dir().path_join("opennova editor thumbnail %d" % Time.get_ticks_usec())
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


## A 2 x 2 uncompressed 32-bit bottom-up TGA: the file's texels B, G, R, A in the order they are stored,
## texel k's red 10 + k, green 100 + k, blue 200 + k, alpha 50 + 60 k.
func _tga() -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes[12] = 2
	bytes[14] = 2
	bytes[16] = 32
	bytes[17] = 8
	for k in 4:
		bytes.append_array(PackedByteArray([200 + k, 100 + k, 10 + k, 50 + 60 * k]))
	return bytes


func test_a_thumbnail_is_uploaded_as_the_cache_made_it() -> void:
	if _app == null:
		return
	var root := OS.get_cache_dir().path_join("opennova editor thumbnail project %d" % Time.get_ticks_usec())
	_dirs.append(root)
	assert_true(_seam.new_project(root, "Thumbnails"))
	_write(root.path_join("textures/brick.tga"), _tga())
	_write(root.path_join("notes.txt"), "a note".to_utf8_buffer())
	_seam.request({"kind": "rescan"})
	var texture: ImageTexture = _app.get_thumbnail_texture("textures/brick.tga", "none")
	assert_not_null(texture, "a texture of the project has a thumbnail")
	if texture == null:
		return
	assert_eq(texture.get_size(), Vector2(2, 2), "small enough to be its own size")
	var image := texture.get_image()
	assert_eq(image.get_pixel(0, 1), Color8(10, 100, 200, 50), "the file's first texel the bottom row's first")
	assert_eq(image.get_pixel(1, 0), Color8(13, 103, 203, 230), "its last the top row's last")
	assert_eq(_app.get_thumbnail_texture("brick.tga", "none"), texture, "one texture a picture, by path or name")
	# The HUD's alpha-only art: its alpha alone.
	var alpha: ImageTexture = _app.get_thumbnail_texture("textures/brick.tga", "alpha_only")
	assert_not_null(alpha)
	if alpha != null:
		assert_eq(alpha.get_image().get_pixel(0, 1), Color8(0, 0, 0, 50), "black under its alpha")
	assert_null(_app.get_thumbnail_texture("notes.txt", "none"), "a file that is no texture has none")
	# The wire's picture: the same, as a PNG.
	var answer: Dictionary = _seam.query("texture_thumbnail", {"path": "textures/brick.tga"})
	assert_eq(String(answer.get("state", "")), "ready")
	var png := Image.new()
	assert_eq(png.load_png_from_buffer(Marshalls.base64_to_raw(String(answer.get("png", "")))), OK)
	assert_eq(png.get_pixel(1, 0), Color8(13, 103, 203, 230), "the PNG's texels the same")
