extends GutTest

# The menus on the original's raster (engine menu/menu_frame.h, renderer/d3d9_raster.h):
# a draw list is the original's window coordinates, which Direct3D 9 rasterises with
# pixel centres on the integers. Every pin renders a real MenuFrame through the device:
# a tests/windowed/ script, run by `scripts/test_godot.sh --suite core --windowed` (a
# headless run pends it).
# - The edges: each the float product of the design point and the scale pair, truncated
#   (the x87 unit at Direct3D 9's single precision), and an outline lit as Direct3D 9's
#   line list lights it, every corner included
#   [orig: CUIElement_DrawOutlineRect @ 0x647fc0; draw_line_2d @ 0x6786d0].
# - An image: the strip's half texel at both ends of its UVs, so a 1:1 image samples each
#   texel's centre and draws crisp [orig: Render_DrawTiledTextureStrip @ 0x67b058].
# - A glyph: the drawer's half-texel bottom bias in the quad's height too, so its last ink
#   row draws at full strength (D-FNT-6) [orig: CGameFont_DrawText @ 0x675733].

const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"
const MNU_TEXT := """
<SCREEN>
  <NAME>RASTER</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <APPEARANCE type="color" state="default">FF000000</APPEARANCE>
    <FONT><NAME>Gunpl22b.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="static" name="BOX">
      <POSITION><LEFT>200</LEFT><TOP>150</TOP><RIGHT>600</RIGHT><BOTTOM>420</BOTTOM></POSITION>
      <APPEARANCE type="outline" state="default">FFFFFFFF</APPEARANCE>
    </WINDOW>
    <WINDOW type="static" name="IMG">
      <POSITION><LEFT>40</LEFT><TOP>40</TOP><RIGHT>56</RIGHT><BOTTOM>48</BOTTOM></POSITION>
      <APPEARANCE type="image" state="default">chk.tga</APPEARANCE>
    </WINDOW>
    <WINDOW type="static" name="TXT">
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>130</BOTTOM></POSITION>
      <STRING justify="LEFT">H</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


# A BGRA TGA checkerboard: texel (x, y) white where x + y is even, black otherwise.
func _checker_tga(size: Vector2i) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18 + size.x * size.y * 4)
	bytes[2] = 2
	bytes.encode_u16(12, size.x)
	bytes.encode_u16(14, size.y)
	bytes[16] = 32
	bytes[17] = 0x28
	for y in size.y:
		for x in size.x:
			var on := (x + y) % 2 == 0
			var at := 18 + (y * size.x + x) * 4
			for c in 3:
				bytes[at + c] = 255 if on else 0
			bytes[at + 3] = 255
	return bytes


# The screen rendered on a size.x x size.y surface through the real MenuFrame.
func _render_menu(size: Vector2i) -> Image:
	var dir := TestFs.cache_dir(self, "menu_d3d9_raster")
	_temp_dirs.append(dir)
	TestFs.copy(self, FONT_FIXTURE, dir.path_join("Gunpl22b.fnt"))
	TestFs.write_bytes(self, dir.path_join("chk.tga"), _checker_tga(Vector2i(16, 8)))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(MNU_TEXT.to_utf8_buffer()), OK)
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var frame := MenuFrame.new()
	frame.size = Vector2(size)
	viewport.add_child(frame)
	assert_true(frame.configure(doc, "", root, null, null))
	await get_tree().process_frame
	await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


func _lit(image: Image, x: int, y: int) -> bool:
	return image.get_pixel(x, y).r > 0.5


# At 1024 x 768 the pair is 1024 x 0.00125f; 200, 150 and 600 scale to 255.99999,
# 191.99999 and 767.99998 in double, and the single-precision products 256, 192 and 768
# are what truncate: the outline runs on columns 256 and 767 and rows 192 and 536, the
# last segment's corner lit as the line list lights it.
func test_the_outline_lands_on_the_game_s_pixels() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var image: Image = await _render_menu(Vector2i(1024, 768))
	for y in [200, 300, 500]:
		assert_true(_lit(image, 256, y), "the left edge on column 256 (row %d)" % y)
		assert_false(_lit(image, 255, y), "nothing on column 255 (row %d)" % y)
		assert_true(_lit(image, 767, y), "the right edge on column 767 (row %d)" % y)
		assert_false(_lit(image, 768, y), "nothing on column 768 (row %d)" % y)
	for x in [300, 500, 700]:
		assert_true(_lit(image, x, 192), "the top edge on row 192 (column %d)" % x)
		assert_false(_lit(image, x, 191), "nothing on row 191 (column %d)" % x)
		assert_true(_lit(image, x, 536), "the bottom edge on row 536 (column %d)" % x)
		assert_false(_lit(image, x, 537), "nothing on row 537 (column %d)" % x)
	for corner in [Vector2i(256, 192), Vector2i(767, 192), Vector2i(767, 536), Vector2i(256, 536)]:
		assert_true(_lit(image, corner.x, corner.y), "the corner %s" % corner)


# At 800 x 600 an image draws 1:1: with the strip's half texel each pixel samples one
# texel's centre, so the checker stays black and white; without it, on this raster's
# shifted centres, every pixel would filter two texels to one grey.
func test_a_one_to_one_image_samples_texel_centres() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var image: Image = await _render_menu(Vector2i(800, 600))
	# The reader's row order is its own; each pixel is one texel, black or white, and
	# its right-hand neighbour the other.
	for y in range(40, 48):
		for x in range(40, 56):
			var value := image.get_pixel(x, y).r
			if value > 0.05 and value < 0.95:
				fail_test("pixel (%d, %d) = %.3f: the image filtered across texels" % [x, y, value])
				return
			if x < 55 and (value > 0.5) == (image.get_pixel(x + 1, y).r > 0.5):
				fail_test("pixels (%d, %d) and (%d, %d) match: the checker is gone" % [x, y, x + 1, y])
				return
	assert_lt(image.get_pixel(39, 44).r, 0.05, "nothing left of the image")
	assert_lt(image.get_pixel(56, 44).r, 0.05, "nothing right of it")
	pass_test("every pixel of the 1:1 checker is one texel")


# The synthetic font's "H" has ink rows 1..14 of its 16-row cell. The glyph quad is the
# 16 rows and the drawer's half-texel bias tall, one texel per pixel, so the last ink row
# draws as bright as the middle one and the empty row under it stays dark (D-FNT-6).
func test_a_glyph_s_last_ink_row_draws_at_full_strength() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var image: Image = await _render_menu(Vector2i(800, 600))
	# The label's anchor is (100, 100); the stem is the cell's columns 2 and 3.
	var middle := image.get_pixel(102, 108).r
	var last := image.get_pixel(102, 114).r
	assert_gt(middle, 0.9, "the stem draws white (the sink's halved white, doubled)")
	assert_almost_eq(last, middle, 0.02, "the last ink row at the middle row's strength")
	assert_lt(image.get_pixel(102, 115).r, 0.05, "the cell's empty last row stays dark")
	assert_lt(image.get_pixel(102, 100).r, 0.05, "the cell's empty first row stays dark")
	assert_almost_eq(image.get_pixel(102, 101).r, middle, 0.02, "the first ink row is whole too")
