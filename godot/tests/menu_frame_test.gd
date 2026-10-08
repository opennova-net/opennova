extends GutTest

# The MenuFrame native class (the compiled-menu device leg over
# engine/runtime/menu MenuFrameCompiler) registers, configures from a parsed
# MnuDocument screen, and compiles a draw list. Layout-only: no resource root,
# so the color fill quad and the outline lines compile while glyphs (which need
# a .fnt) stay zero.

const MNU_TEXT := """
<SCREEN>
  <NAME>SMOKE</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <APPEARANCE type="color" state="default">102030</APPEARANCE>
    <WINDOW type="button" name="OK">
      <POSITION><LEFT>10</LEFT><TOP>20</TOP><RIGHT>110</RIGHT><BOTTOM>40</BOTTOM></POSITION>
      <APPEARANCE type="outline" state="default">FFFFFF</APPEARANCE>
      <STRING justify="CENTER">OK</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


func test_menu_frame_registers_and_compiles_a_layout_only_draw_list() -> void:
	assert_true(ClassDB.class_exists("MenuFrame"), "MenuFrame native class registers")
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(MNU_TEXT.to_utf8_buffer()), OK, "the smoke screen parses")
	var frame := MenuFrame.new()
	assert_true(frame.configure(doc, "", null, null, null), "MenuFrame.configure accepts the screen")
	var stats := frame.get_draw_list_stats()
	assert_eq(stats.widgets_drawn, 2, "the window and its button both draw: %s" % [stats.to_json_value()])
	assert_true(stats.quads >= 1, "the color fill compiles a quad: %s" % [stats.to_json_value()])
	assert_eq(stats.lines, 4, "the outline compiles four lines: %s" % [stats.to_json_value()])
	assert_eq(stats.glyphs, 0, "no .fnt, so no glyphs: %s" % [stats.to_json_value()])
	frame.free()


# A widget's text as the game draws it (the game MCP's game_menu reads it): a retail menu is
# Windows-1252 bytes, each byte a glyph, so the VERSION line's 0xA9 reads as the copyright
# sign, never as U+FFFD.
func test_widget_text_reads_as_the_code_page() -> void:
	var bytes := MNU_TEXT.replace(">OK<", ">\u0001 2004<").to_utf8_buffer()
	var mark := bytes.find(1)
	assert_gt(mark, 0)
	bytes[mark] = 0xA9 # the cp1252 copyright sign, no byte order mark: the code page
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(bytes), OK)
	var frame := MenuFrame.new()
	assert_true(frame.configure(doc, "", null, null, null))
	var ok := -1
	for i in frame.widget_count():
		if frame.widget_name(i) == "OK":
			ok = i
	assert_gt(ok, -1, "the button")
	assert_eq(frame.get_widget_text(ok), "© 2004")
	assert_false(frame.get_widget_text(ok).contains("�"))
	frame.free()


const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"
const MNU_FONT_TEXT := """
<SCREEN>
  <NAME>SMOKE</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <FONT><NAME>Gunpl22b.fnt</NAME><DEFAULT_FG>FFFFFF</DEFAULT_FG></FONT>
    <WINDOW type="button" name="OK">
      <POSITION><LEFT>10</LEFT><TOP>20</TOP><RIGHT>210</RIGHT><BOTTOM>60</BOTTOM></POSITION>
      <STRING justify="CENTER">OK</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


# A menu text draws through its font page's material, 0x651: the text sink halves
# the colour on a modulate-2x device and the page's MODULATE2X doubles it back, so
# white reads 254 (D-HUD-51). The menu items' shader strips the -16 UV.y flag a
# glyph run carries [orig: CFontCache_DrawTextScaled @0x653170 — (c >> 1) &
# 0x7F7F7F @0x6531eb under g_UIHalfBrightMode; GameFont_LoadFromBlob @0x674825].
func test_menu_text_draws_through_the_font_pages_modulate2x() -> void:
	var shader := MenuFrame.glyph_shader_code()
	assert_true(shader.contains("if (UV.y <= -8.0) {\n\t\tUV.y += 16.0;\n\t\tmodulate2x_on = 1.0;"),
			"the menu shader strips the -16 UV.y flag")
	assert_true(shader.contains(
			"if (modulate2x_on > 0.5) {\n\t\tCOLOR.rgb = min(COLOR.rgb * 2.0, vec3(1.0));"),
			"the flagged run's colour doubles, saturated")
	var dir := TestFs.cache_dir(self, "menu_frame_glyphs")
	TestFs.copy(self, FONT_FIXTURE, dir.path_join("Gunpl22b.fnt"))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(MNU_FONT_TEXT.to_utf8_buffer()), OK)
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	assert_true(frame.configure(doc, "", root, null, null))
	assert_gt(frame.get_draw_list_stats().glyphs, 0, "the label lays out through the real .fnt")
	var rows := frame.get_glyph_submissions()
	assert_gt(rows.size(), 0, "the label submits its glyph run")
	for row: Dictionary in rows:
		for uv: Vector2 in row.uvs:
			assert_lte(uv.y, -8.0, "every glyph vertex carries the flag")
		for c: Color in row.colors:
			assert_eq(c.to_argb32(), 0xFF7F7F7F, "white's diffuse is the sink's halved white")
	TestFs.remove_dir_recursive(dir)
