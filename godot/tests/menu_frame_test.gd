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
