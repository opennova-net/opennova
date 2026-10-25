extends GutTest

# A frame border piece's material on the device (D-NET-NEW-LF-25): as the game's device draws
# a piece (engine menu/menu_frame.h, the MenuQuad contract), its quad goes to the server with
# its stencil and its brush bound in one two-stage material, a server canvas texture made once
# per texture pair and reused by every piece and every later draw, so a piece first shown
# costs its draw alone. A server with a RenderingDevice makes that material; the headless
# dummy server hands back an empty RID: a tests/windowed/ script, run by
# `scripts/test_godot.sh --windowed` (a headless run pends it). The pieces' structure (their
# count, their stencil and brush, the diffuse, the stencil UVs) is pinned headless in
# tests/menu_frame_composition_test.gd.

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


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


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


func test_the_pieces_share_one_material_made_once() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var dir := TestFs.cache_dir(self, "menu_frame_piece_material")
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
	var pieces := frame.get_frame_piece_submissions()
	assert_eq(pieces.size(), 8, "the box's eight border pieces reach the server")
	var material := RID()
	for piece in pieces:
		var row := piece as Dictionary
		assert_true((row["material"] as RID).is_valid(), "each piece binds its material")
		if material.is_valid():
			assert_eq(row["material"], material, "the pieces share the pair's one material")
		material = row["material"]
	# A second draw makes no new material.
	var again := frame.get_frame_piece_submissions()
	assert_eq(again.size(), 8)
	assert_eq((again[0] as Dictionary)["material"], material, "the second draw reuses the material")
