extends GutTest

# DRAW_FRAME gating: a window draws a frame ONLY when DRAW_FRAME is set. A window
# may carry a <FRAME> block purely to hand its textures down to framed descendants
# without drawing one itself - the canonical case is the root MAIN window, which
# defines the camo BOXTILE brush + BORDER2 stencil but has NO DRAW_FRAME, so it must
# draw no frame (the bug was a full-window camo background on the in-game ESC menu).
# [orig: CStaticWnd_Render @ 0x657b10 -> field +0x134 guards CUIElement_DrawFrame].
#
# Built in EDIT mode so an unresolved frame still leaves a FramePlaceholder node:
# real game TGAs are absent headless, and at runtime an unresolved frame draws
# nothing, which would make the gate untestable. The check covers both the resolved
# (FrameFill/Frame* pieces) and unresolved (FramePlaceholder) outcomes.

const FRAME_NODE_NAMES := [
	"FramePlaceholder", "FrameFill", "FrameTL", "FrameTop", "FrameTR",
	"FrameLeft", "FrameRight", "FrameBL", "FrameBottom", "FrameBR",
]


func _find_by_name(node: Node, target: String) -> Node:
	if node.name == target:
		return node
	for c in node.get_children():
		var hit := _find_by_name(c, target)
		if hit != null:
			return hit
	return null


# Does this container draw a frame? Checks its DIRECT children only (a frame is
# emitted as children of the window's own container, not a descendant's).
func _has_frame_node(container: Node) -> bool:
	for c in container.get_children():
		if FRAME_NODE_NAMES.has(String(c.name)):
			return true
	return false


func _build_jo_game_edit() -> NovaMnuMenu:
	var path := "res://../fixtures/mnu/jo_game.mnu"
	var bytes := FileAccess.get_file_as_bytes(path)
	assert_gt(bytes.size(), 0, "jo_game.mnu readable")

	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(bytes), OK, "jo_game.mnu parses")

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(true)
	menu.menu = doc
	return menu


# MAIN defines a <FRAME> (camo) but has no DRAW_FRAME -> it must NOT draw a frame.
func test_main_without_draw_frame_draws_no_frame() -> void:
	var menu := _build_jo_game_edit()
	var main := _find_by_name(menu, "MAIN")
	assert_not_null(main, "MAIN window node exists")
	if main != null:
		assert_false(_has_frame_node(main),
				"MAIN has no DRAW_FRAME, so it must draw no frame (no full-window camo)")


# MAIN_WRAPPER carries DRAW_FRAME (inheriting MAIN's textures) -> it DOES draw a frame.
func test_draw_frame_window_draws_a_frame() -> void:
	var menu := _build_jo_game_edit()
	var wrapper := _find_by_name(menu, "MAIN_WRAPPER")
	assert_not_null(wrapper, "MAIN_WRAPPER window node exists")
	if wrapper != null:
		assert_true(_has_frame_node(wrapper),
				"MAIN_WRAPPER has DRAW_FRAME, so it draws the inherited frame")


func _write_png(path: String, w: int, h: int, c: Color) -> void:
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	img.fill(c)
	img.save_png(path)


# The faithful reproduction of the reported bug at the REAL game render path
# (edit_mode == false) with the camo brush actually RESOLVING - exactly the
# condition in the user's screenshot, where BOXTILE.tga loaded and tiled across
# the whole window. With the gate, MAIN (no DRAW_FRAME) draws no frame fill, so
# there is no full-window camo; the DRAW_FRAME box (MAIN_WRAPPER) still fills.
func test_runtime_resolved_camo_does_not_fill_window() -> void:
	var dir := OS.get_temp_dir().path_join("mnu_frame_rt_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	_write_png(dir.path_join("border2.png"), 64, 64, Color(0.7, 0.7, 0.7, 1.0))  # stencil
	_write_png(dir.path_join("boxtile.png"), 8, 8, Color(0.3, 0.4, 0.2, 1.0))    # camo brush

	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		pass_test("temp resource root unavailable: %s" % root.get_last_error())
		return

	var bytes := FileAccess.get_file_as_bytes("res://../fixtures/mnu/jo_game.mnu")
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(bytes), OK, "jo_game.mnu parses")

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_resource_root(root)
	menu.set_edit_mode(false)  # the running game's path
	menu.menu = doc

	var main := _find_by_name(menu, "MAIN")
	assert_not_null(main, "MAIN window node exists")
	if main != null:
		assert_false(_has_frame_node(main),
				"runtime: MAIN (no DRAW_FRAME) draws no camo frame across the window")

	var wrapper := _find_by_name(menu, "MAIN_WRAPPER")
	assert_not_null(wrapper, "MAIN_WRAPPER window node exists")
	if wrapper != null:
		assert_true(_has_frame_node(wrapper),
				"runtime: the DRAW_FRAME box renders the inherited camo fill")

	for f in ["border2.png", "boxtile.png"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)
