extends SceneTree

# Headless probe: the MenuFrame native class (the compiled-menu device leg
# over engine/runtime/menu MenuFrameCompiler) registers, configures from a
# parsed MnuDocument screen, and compiles a draw list (no assets: layout-only
# quad/line counts). CLI driver, not a GUT test:
#   "$GODOT_BIN" --headless --path godot -s res://tests/menu_frame_probe.gd

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


func _init() -> void:
	var ok := true
	if not ClassDB.class_exists("MenuFrame"):
		push_error("MenuFrame class not registered")
		quit(1)
		return
	var doc := MnuDocument.new()
	var parse_err: Error = doc.load_from_bytes(MNU_TEXT.to_utf8_buffer())
	if parse_err != OK:
		push_error("fixture parse failed: %d" % parse_err)
		ok = false
	var frame := MenuFrame.new()
	var configured: bool = frame.configure(doc, "", null, null, {})
	if not configured:
		push_error("MenuFrame.configure failed")
		ok = false
	var stats: Dictionary = frame.get_draw_list_stats()
	# Layout-only: the color fill quad + the outline lines compile; glyphs
	# need a .fnt so they stay zero without a resource root.
	if int(stats.get("widgets_drawn", 0)) != 2:
		push_error("expected 2 widgets drawn, got %s" % [stats])
		ok = false
	if int(stats.get("quads", 0)) < 1 or int(stats.get("lines", 0)) != 4:
		push_error("unexpected draw list: %s" % [stats])
		ok = false
	frame.free()
	print("menu_frame_probe: %s %s" % ["PASS" if ok else "FAIL", stats])
	quit(0 if ok else 1)
