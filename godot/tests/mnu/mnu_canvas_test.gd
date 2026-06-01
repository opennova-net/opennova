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


func test_pick_stack_orders_top_to_bottom() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id := _first_root_child(doc, 1)  # StartBtn, over the root window
	var stack = canvas.pick_stack_at(Vector2(320, 135))
	assert_gt(stack.size(), 1, "Overlapping widgets produce a multi-entry stack.")
	assert_eq(stack[0], start_id, "The topmost (the child) is first; matches pick_widget_at.")
	assert_true(stack.has(root), "The stack also includes the overlapping ancestor.")


func test_repeated_click_cycles_overlapping_stack() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id := _first_root_child(doc, 1)
	# Successive clicks at ~the same point step down the z-stack, then wrap.
	assert_eq(canvas._pick_with_cycle(Vector2(320, 135)), start_id, "First pick = topmost.")
	assert_eq(canvas._pick_with_cycle(Vector2(320, 135)), root, "Second pick at the same point = next down.")
	assert_eq(canvas._pick_with_cycle(Vector2(321, 135)), start_id, "Within tolerance keeps cycling; wraps to top.")
	# A click past the tolerance (still over both widgets) restarts at the top.
	assert_eq(canvas._pick_with_cycle(Vector2(340, 145)), start_id, "Moving past the tolerance resets to the topmost.")


func test_walk_ids_collects_subtree_excluding_screens() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var w1 := doc.add_widget(root, NovaMnuDocument.TYPE_WINDOW, Rect2(0, 0, 100, 100))
	doc.add_widget(w1, NovaMnuDocument.TYPE_STATIC, Rect2(0, 0, 10, 10))
	var ids := PackedInt32Array()
	MnuCanvasScript._walk_ids(doc, root, ids)
	assert_true(ids.has(root), "Includes the root window.")
	assert_true(ids.has(w1), "Includes a nested window.")
	assert_gt(ids.size(), 2, "Collects the whole subtree, not just the root.")
	var from_screen := PackedInt32Array()
	MnuCanvasScript._walk_ids(doc, doc.get_screen_ids()[0], from_screen)
	assert_false(from_screen.has(doc.get_screen_ids()[0]), "Screen ids are excluded.")
	assert_true(from_screen.has(root), "Walking from the screen still collects its root window.")


# --- Zoom / pan -----------------------------------------------------------------

func test_zoom_to_cursor_keeps_point_fixed() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	# The board point under the cursor must stay under the cursor across a zoom step.
	var cursor := Vector2(450, 200)
	var before = canvas._canvas_to_board(cursor)
	canvas._zoom_at(cursor, 2.0)
	var after = canvas._canvas_to_board(cursor)
	assert_almost_eq(after.x, before.x, 0.01, "Zoom keeps the board x under the cursor.")
	assert_almost_eq(after.y, before.y, 0.01, "Zoom keeps the board y under the cursor.")


func test_transform_round_trips_under_zoom_and_pan() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	canvas._zoom_at(Vector2(320, 240), 3.0)  # zoom in at the board center
	var p := Vector2(123, 77)
	var rt = canvas._board_to_canvas(canvas._canvas_to_board(p))
	assert_almost_eq(rt.x, p.x, 0.01, "canvas->board->canvas round-trips under zoom (x).")
	assert_almost_eq(rt.y, p.y, 0.01, "canvas->board->canvas round-trips under zoom (y).")


func test_reset_view_restores_identity_fit() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	canvas._zoom_at(Vector2(200, 200), 4.0)
	canvas.reset_view()
	# Back to the 640x480 identity fit: a canvas point equals its board point.
	assert_almost_eq(canvas._canvas_to_board(Vector2(300, 150)).x, 300.0, 0.01, "Fit reset restores identity x.")
	assert_almost_eq(canvas._canvas_to_board(Vector2(300, 150)).y, 150.0, 0.01, "Fit reset restores identity y.")


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


# --- Off-board placement guard --------------------------------------------------

func test_rect_off_board_pure_helper() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]  # board derived to 640x480
	assert_false(canvas._rect_off_board(Rect2(10, 10, 100, 30)), "A rect fully inside the board is on-board.")
	assert_true(canvas._rect_off_board(Rect2(600, 10, 100, 30)), "A rect overhanging the right edge is off-board.")
	assert_true(canvas._rect_off_board(Rect2(10, 470, 40, 40)), "A rect overhanging the bottom edge is off-board.")
	assert_true(canvas._rect_off_board(Rect2(-5, 10, 20, 20)), "A negative origin is off-board.")
	assert_false(canvas._rect_off_board(Rect2(50, 50, 0, 0)), "A zero-size rect is skipped (not flagged).")
	assert_false(canvas._rect_off_board(Rect2(0, 0, 640, 480)), "An exact board-fit is on-board (inclusive).")


func test_is_selection_off_board_tracks_selection() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # StartBtn (270,120,100,30), on-board
	canvas.set_selected(start_id)
	assert_false(canvas.is_selection_off_board(), "An on-board selection is not flagged.")
	doc.set_window_rect(start_id, Rect2(600, 120, 100, 30))  # right edge 700 > 640
	canvas.set_selected(start_id)
	assert_true(canvas.is_selection_off_board(), "A selection pushed past the edge is flagged.")


# --- Phase 4: multi-select + marquee --------------------------------------------

# A canvas over the fixture with two extra, known, non-overlapping widgets added under
# MAIN's root (so picking by their centers is deterministic regardless of fixture
# layout). Returns [canvas, doc, a, b]. The root sits at the origin, so each added
# widget's absolute rect equals its local rect.
func _canvas_with_pair() -> Array:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var a := doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(20, 360, 80, 30))   # center (60,375)
	var b := doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(500, 360, 80, 30))  # center (540,375)
	return [canvas, doc, a, b]


func test_ids_in_marquee_selects_intersecting() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var win := doc.add_widget(root, NovaMnuDocument.TYPE_WINDOW, Rect2(100, 100, 200, 200))
	var child := doc.add_widget(win, NovaMnuDocument.TYPE_STATIC, Rect2(10, 10, 50, 30))  # abs (110,110,50,30)
	var far := doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(500, 400, 40, 20))
	var hits := MnuCanvasScript._ids_in_marquee(doc, root, Rect2(90, 90, 150, 150))
	assert_true(hits.has(win), "A window intersecting the box is selected.")
	assert_true(hits.has(child), "A nested child intersecting the box is selected (absolute-rect math).")
	assert_false(hits.has(far), "A widget outside the box is not selected.")
	assert_false(hits.has(root), "The background root window is not a marquee target.")
	assert_false(hits.has(doc.get_screen_ids()[0]), "Screen ids are never included.")


func test_shift_click_toggles_membership() -> void:
	var arr = await _canvas_with_pair()
	var canvas = arr[0]
	var a: int = arr[2]
	var b: int = arr[3]
	canvas._on_press(Vector2(60, 375))    # plain select A
	canvas._on_release()
	assert_eq(canvas._selection, PackedInt32Array([a]), "Plain click selects only A.")
	watch_signals(canvas)
	canvas._on_press(Vector2(540, 375), true)   # shift-add B
	assert_eq(canvas._selection.size(), 2, "Shift-click adds a second widget.")
	assert_true(canvas._selection.has(a) and canvas._selection.has(b), "Both A and B are selected.")
	assert_signal_emitted(canvas, "selection_set", "A toggle announces the new set.")
	canvas._on_press(Vector2(540, 375), true)   # shift-click B again removes it
	assert_eq(canvas._selection, PackedInt32Array([a]), "Toggling a member off leaves the rest.")


func test_plain_click_replaces_multi_selection() -> void:
	var arr = await _canvas_with_pair()
	var canvas = arr[0]
	var doc: NovaMnuDocument = arr[1]
	var a: int = arr[2]
	var b: int = arr[3]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var c := doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(250, 360, 80, 30))  # center (290,375)
	canvas.set_selection(PackedInt32Array([a, b]))
	watch_signals(canvas)
	canvas._on_press(Vector2(290, 375))   # plain click on a non-member
	canvas._on_release()
	assert_eq(canvas._selection, PackedInt32Array([c]), "A plain click on a non-member replaces the selection.")
	assert_signal_emit_count(canvas, "widget_picked", 1, "Replacing emits one widget_picked.")


func test_click_member_no_drag_collapses() -> void:
	var arr = await _canvas_with_pair()
	var canvas = arr[0]
	var a: int = arr[2]
	var b: int = arr[3]
	canvas.set_selection(PackedInt32Array([a, b]))
	watch_signals(canvas)
	canvas._on_press(Vector2(60, 375))   # press on member A
	canvas._on_release()                  # no drag -> collapse to A
	assert_eq(canvas._selection, PackedInt32Array([a]), "Clicking a member without dragging collapses to it.")
	assert_signal_emit_count(canvas, "widget_picked", 1, "Collapsing emits one widget_picked.")
	assert_signal_emit_count(canvas, "rect_committed_batch", 0, "A no-drag collapse commits no move.")


func test_marquee_drag_selects_and_emits() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var start_id := _first_root_child(doc, 1)  # StartBtn (270,120,100,30)
	watch_signals(canvas)
	# (10,100) is a proven root-only point (see test_pick_prefers_child_over_root); the
	# box drags across StartBtn.
	canvas._on_press(Vector2(10, 100))         # over the background -> arm a marquee
	canvas._on_drag(Vector2(380, 160), false)  # box covers StartBtn
	canvas._on_release()
	assert_signal_emitted(canvas, "selection_set", "A marquee drag emits selection_set.")
	assert_true(canvas._selection.has(start_id), "The marquee selects the boxed widget.")


func test_empty_click_no_drag_clears() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	canvas.set_selected(_first_root_child(doc, 1))
	watch_signals(canvas)
	canvas._on_press(Vector2(-50, -50))   # truly empty (outside every widget)
	canvas._on_release()                   # no drag
	assert_signal_emit_count(canvas, "selection_cleared", 1, "An empty click with no drag clears (preserved).")
	assert_signal_emit_count(canvas, "selection_set", 0, "An empty click is not a marquee selection.")
	assert_eq(canvas._selection.size(), 0, "The selection is empty after an empty click.")


func test_group_move_emits_one_batch() -> void:
	var arr = await _canvas_with_pair()
	var canvas = arr[0]
	var a: int = arr[2]   # (20,360,80,30)
	var b: int = arr[3]   # (500,360,80,30)
	canvas.set_selection(PackedInt32Array([a, b]))
	watch_signals(canvas)
	canvas._on_press(Vector2(60, 375))            # press on member A -> arm group move
	canvas._on_drag(Vector2(100, 399), true)      # +40,+24 (Alt = no snap)
	canvas._on_release()
	assert_signal_emit_count(canvas, "rect_committed_batch", 1, "A group move commits exactly one batch.")
	var edits = get_signal_parameters(canvas, "rect_committed_batch", 0)[0]
	assert_eq(edits.size(), 2, "Both members are in the batch.")
	var by_id := {}
	for e in edits:
		by_id[e["id"]] = e["rect"]
	assert_eq(by_id[a], Rect2(60, 384, 80, 30), "A's local rect shifted by the delta.")
	assert_eq(by_id[b], Rect2(540, 384, 80, 30), "B's local rect shifted by the same delta.")


func test_group_move_carries_nested_child() -> void:
	var pair = await _canvas_with_fixture()
	var canvas = pair[0]
	var doc: NovaMnuDocument = pair[1]
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var parent := doc.add_widget(root, NovaMnuDocument.TYPE_WINDOW, Rect2(100, 100, 200, 150))  # center (200,175)
	var child := doc.add_widget(parent, NovaMnuDocument.TYPE_STATIC, Rect2(20, 20, 60, 30))      # local
	canvas.set_selection(PackedInt32Array([parent, child]))
	watch_signals(canvas)
	canvas._on_press(Vector2(200, 175))           # press on the parent (over no child here)
	canvas._on_drag(Vector2(232, 191), true)      # +32,+16 (Alt = no snap)
	canvas._on_release()
	var edits = get_signal_parameters(canvas, "rect_committed_batch", 0)[0]
	var by_id := {}
	for e in edits:
		by_id[e["id"]] = e["rect"]
	assert_eq(by_id[parent], Rect2(132, 116, 200, 150), "The parent shifts by the delta.")
	assert_eq(by_id[child], Rect2(20, 20, 60, 30), "The selected child is carried (local rect unchanged).")
