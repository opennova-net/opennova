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
	assert_true(frame.configure(doc, "", null, null, {}), "MenuFrame.configure accepts the screen")
	var stats: Dictionary = frame.get_draw_list_stats()
	assert_eq(int(stats.get("widgets_drawn", 0)), 2, "the window and its button both draw: %s" % [stats])
	assert_true(int(stats.get("quads", 0)) >= 1, "the color fill compiles a quad: %s" % [stats])
	assert_eq(int(stats.get("lines", 0)), 4, "the outline compiles four lines: %s" % [stats])
	assert_eq(int(stats.get("glyphs", 0)), 0, "no .fnt, so no glyphs: %s" % [stats])
	frame.free()
