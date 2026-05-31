extends GutTest

# M8a gate: the Menus canvas gesture layer — picking (with absolute-rect math for
# nested widgets), move/resize gestures with grid + edge snapping, and arrow-key
# nudging. The gesture state machine is exercised through _on_press / _on_drag /
# _on_release (the _gui_input dispatcher just forwards to these and calls
# accept_event), and the pure snap helpers are unit-tested directly.

const MnuCanvasScript = preload("res://modtools/mnu/mnu_canvas.gd")
const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _first_root_child(doc: NovaMnuDocument, index: int) -> int:
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	return doc.get_child_ids(root)[index]


# A canvas sized exactly to the 640x480 board, so the letterbox fit is identity
# (scale 1, offset 0): a canvas point equals a board point. Returns [canvas, doc].
func _canvas_with_fixture() -> Array:
	var doc := _load_doc()
	var canvas = MnuCanvasScript.new()
	add_child_autofree(canvas)
	canvas.size = Vector2(640, 480)
	await get_tree().process_frame
	canvas.set_menu(doc, null, null)
	await get_tree().process_frame
	return [canvas, doc]


func _key(code: int, shift := false) -> InputEventKey:
	var k := InputEventKey.new()
	k.keycode = code
	k.pressed = true
	k.shift_pressed = shift
	return k


# --- Picking --------------------------------------------------------------------

func test_pick_widget_at_returns_widget_under_point() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # StartBtn (270,120,100,30)
	assert_eq(canvas.pick_widget_at(Vector2(320, 135)), start_id, "Picks the widget under the point.")


func test_pick_prefers_child_over_root() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id := _first_root_child(doc, 1)
	assert_eq(canvas.pick_widget_at(Vector2(320, 135)), start_id, "A child is picked over its containing root window.")
	assert_eq(canvas.pick_widget_at(Vector2(10, 100)), root, "A point inside the root but over no child picks the root.")


func test_pick_empty_board_returns_screen() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var screen := doc.get_screen_ids()[0]
	assert_eq(canvas.pick_widget_at(Vector2(-50, -50)), screen, "A point outside every widget returns the screen id.")


func test_abs_rect_of_nested_widget() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var w1 := doc.add_widget(root, NovaMnuDocument.TYPE_WINDOW, Rect2(100, 100, 200, 200))
	var w2 := doc.add_widget(w1, NovaMnuDocument.TYPE_STATIC, Rect2(10, 10, 50, 30))
	assert_eq(doc.get_window_rect(w2), Rect2(10, 10, 50, 30), "The local rect is parent-relative.")
	assert_eq(canvas._abs_rect_of(w2), Rect2(110, 110, 50, 30), "The absolute rect sums ancestor offsets (deep nesting).")


# --- Move / resize gestures -----------------------------------------------------

func test_drag_moves_widget_and_emits_one_commit() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # (270,120,100,30)
	watch_signals(canvas)
	canvas._on_press(Vector2(320, 135))       # select StartBtn + arm a move
	canvas._on_drag(Vector2(350, 145), true)  # +30,+10, Alt = no snap
	canvas._on_release()
	assert_signal_emit_count(canvas, "rect_committed", 1, "A drag commits exactly once.")
	var params = get_signal_parameters(canvas, "rect_committed", 0)
	assert_eq(params[0], start_id, "The commit carries the widget id.")
	assert_eq(params[1], Rect2(300, 130, 100, 30), "The moved local rect (Alt disables snap).")


func test_resize_from_corner_handle() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # (270,120,100,30); BR handle at (370,150)
	canvas.set_selected(start_id)
	watch_signals(canvas)
	canvas._on_press(Vector2(370, 150))        # grab the BR handle
	canvas._on_drag(Vector2(390, 170), true)   # +20,+20, no snap
	canvas._on_release()
	var params = get_signal_parameters(canvas, "rect_committed", 0)
	assert_eq(params[1], Rect2(270, 120, 120, 50), "Resizing from BR grows the size; the position is fixed.")


func test_resize_clamps_to_min_size() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # (270,120,100,30)
	canvas.set_selected(start_id)
	watch_signals(canvas)
	canvas._on_press(Vector2(370, 150))        # BR handle
	canvas._on_drag(Vector2(100, 50), true)    # drag past TL: would invert
	canvas._on_release()
	var params = get_signal_parameters(canvas, "rect_committed", 0)
	assert_eq(params[1], Rect2(270, 120, 4, 4), "Resize clamps to MIN_WIDGET_SIZE without inverting.")


func test_snap_rounds_to_grid() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	watch_signals(canvas)
	canvas._on_press(Vector2(320, 135))        # select StartBtn (x=270)
	canvas._on_drag(Vector2(331, 135), false)  # +11 x, snap ON
	canvas._on_release()
	var params = get_signal_parameters(canvas, "rect_committed", 0)
	assert_eq(params[1].position.x, 280.0, "Snap rounds the x to the 8px grid.")
	assert_eq(int(params[1].position.x) % 8, 0, "The snapped x is grid-aligned.")


func test_alt_disables_snap() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	watch_signals(canvas)
	canvas._on_press(Vector2(320, 135))
	canvas._on_drag(Vector2(331, 135), true)   # +11 x, Alt = no snap
	canvas._on_release()
	var params = get_signal_parameters(canvas, "rect_committed", 0)
	assert_eq(params[1].position.x, 281.0, "Alt disables snap; the exact x is committed.")


func test_bare_click_selects_without_commit() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	watch_signals(canvas)
	canvas._on_press(Vector2(320, 135))  # select StartBtn
	canvas._on_release()                  # no motion -> not a drag
	assert_signal_emit_count(canvas, "rect_committed", 0, "A bare click commits no rect (no undo op).")
	assert_signal_emit_count(canvas, "widget_picked", 1, "A bare click still selects (one widget_picked).")


# --- Keyboard nudge -------------------------------------------------------------

func test_keyboard_nudge_emits_commit() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # (270,120,100,30)
	canvas.set_selected(start_id)
	watch_signals(canvas)
	canvas._unhandled_key_input(_key(KEY_RIGHT))
	assert_eq(get_signal_parameters(canvas, "rect_committed", 0)[1], Rect2(271, 120, 100, 30), "Right arrow nudges +1.")
	canvas._unhandled_key_input(_key(KEY_RIGHT, true))
	assert_eq(get_signal_parameters(canvas, "rect_committed", 1)[1], Rect2(278, 120, 100, 30), "Shift+Right nudges by the grid step.")


func test_keyboard_nudge_guarded_by_screen_selection() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	canvas.set_selected(doc.get_screen_ids()[0])  # a screen, not a widget
	watch_signals(canvas)
	canvas._unhandled_key_input(_key(KEY_RIGHT))
	assert_signal_emit_count(canvas, "rect_committed", 0, "No nudge when a screen is selected.")


# --- Pure snap helper -----------------------------------------------------------

func test_snap_rect_pure_helper() -> void:
	var sz := Vector2(640, 480)
	# Both coords are clear of the board edges, so they round to the grid (24->24,
	# 11->8); a coord within SNAP_EDGE_TOL of 0/extent would snap to the edge instead.
	var moved = MnuCanvasScript._snap_rect(Rect2(11, 21, 100, 30), MnuCanvasScript.Gesture.MOVE, -1, sz, true)
	assert_eq(moved.position, Vector2(8, 24), "MOVE snaps the top-left to the 8px grid.")
	assert_eq(moved.size, Vector2(100, 30), "MOVE preserves the size.")

	var raw = MnuCanvasScript._snap_rect(Rect2(11, 21, 100, 30), MnuCanvasScript.Gesture.MOVE, -1, sz, false)
	assert_eq(raw, Rect2(11, 21, 100, 30), "snap_on = false passes the rect through unchanged.")

	# RESIZE: a moved left edge within tolerance snaps to the board edge (0).
	var rz = MnuCanvasScript._snap_rect(Rect2(3, 100, 100, 30), MnuCanvasScript.Gesture.RESIZE, 6, sz, true)
	assert_eq(rz.position.x, 0.0, "A left edge within tolerance snaps to the board edge.")

	# RESIZE: an edge snap that would cross the fixed edge is re-clamped to
	# MIN_WIDGET_SIZE (the right edge of a tiny rect near the origin must not invert).
	var inv = MnuCanvasScript._snap_rect(Rect2(2, 100, 4, 30), MnuCanvasScript.Gesture.RESIZE, 7, sz, true)
	assert_eq(inv, Rect2(2, 100, 4, 30), "RESIZE edge-snap near the origin clamps to MIN_WIDGET_SIZE (no inversion).")
