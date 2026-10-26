extends GutTest

# A MenuFrame composes a fill tile over its texture's texels in one native pass
# (godot/src/mnu/menu_frame.cpp read_texels, rgba8_from_colors): the texels are read once
# through the engine's conversion to RGBAF, and the composed colours reach RGBA8 through the
# engine's conversion back. Those two conversions are Image's own per-texel get_pixel /
# set_pixel, so the bytes are the ones the per-pixel binding calls wrote; these pins hold the
# engine to that. A frame border piece composes nothing: as the game's device draws it
# (engine menu/menu_frame.h, the MenuQuad contract), the piece's quad goes to the server with
# its stencil and its brush bound in one two-stage material, and the glyph shader's
# frame-piece branch runs the stages, so death.mnu's first shown pieces cost no composition
# (D-NET-NEW-LF-25).

const MNU_FRAMED := """
<SCREEN>
  <NAME>FRAMED</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <APPEARANCE type="color" state="default">102030</APPEARANCE>
    <WINDOW type="static" name="BOX" DRAW_FRAME>
      <POSITION><LEFT>100</LEFT><TOP>80</TOP><RIGHT>500</RIGHT><BOTTOM>380</BOTTOM></POSITION>
      <FRAME>
        <STENCIL size="8">border.tga</STENCIL>
        <BRUSH>tile.tga</BRUSH>
      </FRAME>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


# Every byte value in every channel, laid out over one image of `format`.
func _every_byte_image(format: Image.Format) -> Image:
	var image := Image.create(256, 4, false, format)
	for y in 4:
		for x in 256:
			var b := float((x + y * 67) % 256) / 255.0
			var g := float((x * 7 + y) % 256) / 255.0
			var r := float(x) / 255.0
			var a := float((255 - x + y * 31) % 256) / 255.0
			image.set_pixel(x, y, Color(r, g, b, a))
	return image


func test_the_float_read_is_get_pixel_s_for_every_byte_format() -> void:
	for format in [Image.FORMAT_RGBA8, Image.FORMAT_RGB8, Image.FORMAT_LA8, Image.FORMAT_L8]:
		var source := _every_byte_image(format)
		var floats := source.duplicate() as Image
		floats.convert(Image.FORMAT_RGBAF)
		assert_eq(floats.get_format(), Image.FORMAT_RGBAF)
		var mismatches := 0
		for y in source.get_height():
			for x in source.get_width():
				if floats.get_pixel(x, y) != source.get_pixel(x, y):
					mismatches += 1
		assert_eq(mismatches, 0, "format %d: the RGBAF texels are get_pixel's" % format)


func test_the_rgba8_write_is_set_pixel_s() -> void:
	# Colours on and around every quantisation step, past both ends of the range, and
	# the modulate products a frame piece writes.
	var colors: Array[Color] = []
	for k in 256:
		var step := float(k) / 255.0
		colors.append(Color(step, step - 0.0000001, step + 0.0000001, step * 0.5))
		colors.append(Color(2.0 * step * (127.0 / 255.0), step * step, 1.0 - step, step + 0.5))
	colors.append(Color(-0.25, 1.75, 0.999999, 0.000001))
	var width := colors.size()
	var expected := Image.create(width, 1, false, Image.FORMAT_RGBA8)
	var floats := Image.create(width, 1, false, Image.FORMAT_RGBAF)
	for x in width:
		expected.set_pixel(x, 0, colors[x])
		floats.set_pixel(x, 0, colors[x])
	floats.convert(Image.FORMAT_RGBA8)
	assert_eq(floats.get_data(), expected.get_data(),
			"the RGBAF -> RGBA8 conversion quantises as set_pixel does")


# A BGRA TGA of size whose texels vary in every channel.
func _gradient_tga(size: Vector2i, seed_value: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18 + size.x * size.y * 4)
	bytes[2] = 2
	bytes.encode_u16(12, size.x)
	bytes.encode_u16(14, size.y)
	bytes[16] = 32
	bytes[17] = 0x28
	for y in size.y:
		for x in size.x:
			var at := 18 + (y * size.x + x) * 4
			bytes[at + 0] = (x * 37 + seed_value) % 256
			bytes[at + 1] = (y * 53 + seed_value * 3) % 256
			bytes[at + 2] = (x * y + seed_value * 7) % 256
			bytes[at + 3] = 128 + (x + y) % 128
	return bytes


func test_a_framed_screen_draws_its_pieces_through_their_material() -> void:
	var dir := TestFs.cache_dir(self, "menu_frame_composition")
	_temp_dirs.append(dir)
	TestFs.write_bytes(self, dir.path_join("border.tga"), _gradient_tga(Vector2i(32, 32), 11))
	TestFs.write_bytes(self, dir.path_join("tile.tga"), _gradient_tga(Vector2i(16, 8), 5))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(MNU_FRAMED.to_utf8_buffer()), OK)
	var frame := MenuFrame.new()
	frame.size = Vector2(1600, 1200)
	add_child_autofree(frame)
	assert_true(frame.configure(doc, "", root, null, null))
	assert_true(frame.get_draw_list_stats().quads >= 10,
			"the fill, the box's fill cell and its eight pieces compile")
	var pieces := frame.get_frame_piece_submissions()
	assert_eq(pieces.size(), 8, "the box's eight border pieces reach the server")
	# The material is a server canvas texture, which the headless dummy server does not make
	# (it hands back an empty RID): its sharing and reuse are pinned on the device, by
	# tests/windowed/menu_frame_piece_material_test.gd.
	for piece in pieces:
		var row := piece as Dictionary
		# The stencil and the brush at their own sizes: the shader reads the brush's
		# period off its size (its height across x, its width down y).
		assert_eq((row["stencil"] as Texture2D).get_size(), Vector2(32, 32))
		assert_eq((row["brush"] as Texture2D).get_size(), Vector2(16, 8))
		assert_true((row["color"] as Color).is_equal_approx(Color8(0x7F, 0x7F, 0x7F, 0xFF)),
				"the border diffuse 0xFF7F7F7F")
		var uv := row["uv"] as Rect2
		assert_true(uv.position.x >= 0.0 and uv.end.x <= 1.0 and uv.position.y >= 0.0
				and uv.end.y <= 1.0, "the stencil cell's UVs: %s" % uv)
	# A second draw submits the same eight pieces.
	var again := frame.get_frame_piece_submissions()
	assert_eq(again.size(), 8)


const MNU_PLAIN := """
<SCREEN>
  <NAME>PLAIN</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <APPEARANCE type="color" state="default">102030</APPEARANCE>
  </WINDOW>
</SCREEN>
"""


func test_a_screen_no_longer_shown_lets_its_piece_materials_go() -> void:
	# The pieces' materials are let go as the composed frame textures are (E-13): a
	# pair no draw of the current or the previous configure used is dropped, its server
	# canvas texture freed with it, so the stencil and brush it held are not kept alive.
	var dir := TestFs.cache_dir(self, "menu_frame_piece_prune")
	_temp_dirs.append(dir)
	TestFs.write_bytes(self, dir.path_join("border.tga"), _gradient_tga(Vector2i(32, 32), 11))
	TestFs.write_bytes(self, dir.path_join("tile.tga"), _gradient_tga(Vector2i(16, 8), 5))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var framed := MnuDocument.new()
	assert_eq(framed.load_from_bytes(MNU_FRAMED.to_utf8_buffer()), OK)
	var plain := MnuDocument.new()
	assert_eq(plain.load_from_bytes(MNU_PLAIN.to_utf8_buffer()), OK)
	var frame := MenuFrame.new()
	frame.size = Vector2(1600, 1200)
	add_child_autofree(frame)
	assert_true(frame.configure(framed, "", root, null, null))
	assert_eq(frame.get_frame_piece_submissions().size(), 8)
	assert_eq(frame.get_piece_material_count(), 1, "one stencil and brush pair")
	assert_true(frame.configure(plain, "", root, null, null))
	frame.get_frame_piece_submissions()
	assert_eq(frame.get_piece_material_count(), 1, "the previous configure's pair stays")
	assert_true(frame.configure(plain, "", root, null, null))
	frame.get_frame_piece_submissions()
	assert_eq(frame.get_piece_material_count(), 0, "a pair two configures unused goes")


func test_the_shader_runs_the_two_stage_border_branch() -> void:
	var code := MenuFrame.glyph_shader_code()
	assert_true(code.contains("NORMAL_TEXTURE"), "the brush rides the material's normal slot")
	assert_true(code.contains("UV.x <= -8.0"), "a piece's UV.x carries the flag")
	assert_true(code.contains("brush_size.yx"), "the brush period is its height across x")
