class_name MnuCanvas
extends Control

# WYSIWYG edit surface for the Menus workspace. Hosts a live NovaMnuMenu in
# edit_mode (inert: no navigation, audio, or cursor side effects) scaled with
# uniform letterbox scaling to fit the document's authoring size (currently a
# fixed 640x480 design resolution, the standard for Joint Operations (JO) and
# newer menus; the engine does not yet parse a per-menu resolution).
#
# M8a: the canvas owns layout gestures. It picks the widget under the cursor
# (computing absolute board-space rects so deeply nested widgets pick correctly),
# draws a selection outline + eight resize handles, and lets the user move/resize
# by dragging. During a drag only a "ghost" rect moves (the document is NOT
# mutated per-motion, which would rebuild the whole preview); on release the final
# rect is emitted as one edit so it reuses the editor's existing rect-undo op.
# Snapping is on by default (grid + board edges), bypassed while Alt is held.

const COL_BG := Color(0.07, 0.08, 0.09)
const COL_LETTERBOX := Color(0.12, 0.13, 0.15)
const COL_BORDER := Color(1, 1, 1, 0.08)
const COL_SELECT := Color(0.84, 0.55, 0.29, 0.95)
const COL_GHOST := Color(0.45, 0.78, 1.0, 0.95)
const COL_HANDLE := Color(0.95, 0.95, 0.97, 1.0)

# Gesture tuning (canvas pixels unless noted). Board units are the document's
# authoring coordinates (the doc stores ints, so integer snapping is implicit).
const HANDLE_SIZE := 8.0       # drawn handle square side (canvas px)
const HANDLE_HIT := 10.0       # handle grab tolerance (canvas px)
const DRAG_THRESHOLD := 4.0    # px of motion before a press becomes a drag
const MIN_WIDGET_SIZE := 4.0   # smallest resize extent (board units)
const SNAP_GRID := 8.0         # snap grid step (board units)
const SNAP_EDGE_TOL := 6.0     # snap-to-board-edge tolerance (board units)

enum Gesture { NONE, MOVE, RESIZE }

# Handle order is fixed (indices used by hit-test, draw, snap, and cursor):
# 0 TL, 1 TR, 2 BL, 3 BR (corners), 4 T, 5 B, 6 L, 7 R (edge midpoints).
const _HANDLES_LEFT := [0, 2, 6]
const _HANDLES_RIGHT := [1, 3, 7]
const _HANDLES_TOP := [0, 1, 4]
const _HANDLES_BOTTOM := [2, 3, 5]

signal widget_picked(id: int)
signal selection_cleared()
signal rect_committed(id: int, local_rect: Rect2)

var _preview: NovaMnuMenu
var _document: NovaMnuDocument
var _resource_root: NovaResourceRoot
var _text_resource: RtxtStringFile

var _menu_size := Vector2(640, 480)
var _fit_scale := 1.0
var _fit_offset := Vector2.ZERO
var _visible_screen_name := ""

# Selection + live gesture state. _selected_id is the canvas's own selection (kept
# in sync with the editor via set_selected for programmatic selection, and set
# here directly on a canvas pick). _drag_rect is the ghost in board space.
var _selected_id := -1
var _gesture: int = Gesture.NONE
var _active_handle := -1
var _pressed := false
var _passed_threshold := false
var _press_pos := Vector2.ZERO
var _drag_start_rect := Rect2()
var _drag_rect := Rect2()


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	focus_mode = Control.FOCUS_CLICK
	if not resized.is_connected(_on_resized):
		resized.connect(_on_resized)
	_rebuild_preview()
	_recompute_fit()


# Point the preview at a document. resource_root/text_resource are optional; when
# absent the builder degrades to placeholder visuals (M3 behavior).
func set_menu(doc: NovaMnuDocument, resource_root: NovaResourceRoot, text_resource: RtxtStringFile) -> void:
	_document = doc
	_resource_root = resource_root
	_text_resource = text_resource
	if doc != null:
		_menu_size = Vector2(doc.get_menu_size())
		# Re-seed the visible screen when it is empty OR a stale name the new
		# document lacks, so a document swap never leaves every screen hidden
		# (mirrors NovaMnuMenu::build resetting current_screen_).
		var ids := doc.get_screen_ids()
		if not _doc_has_screen(doc, _visible_screen_name):
			_visible_screen_name = doc.get_screen_name(ids[0]) if ids.size() > 0 else ""
	_rebuild_preview()
	_recompute_fit()
	queue_redraw()


func show_screen_named(screen_name: String) -> void:
	_visible_screen_name = screen_name
	_apply_screen_visibility()


func get_visible_screen_name() -> String:
	return _visible_screen_name


# True when the document has a screen with this (non-empty) name.
func _doc_has_screen(doc: NovaMnuDocument, screen_name: String) -> bool:
	if doc == null or screen_name.is_empty():
		return false
	for sid in doc.get_screen_ids():
		if doc.get_screen_name(sid) == screen_name:
			return true
	return false


# Set the highlighted/selected node by id (programmatic selection from the editor:
# tree click, default selection, undo/redo). A screen or invalid id simply draws no
# outline. The canvas's own picks set _selected_id directly, so this never re-emits.
func set_selected(id: int) -> void:
	_selected_id = id
	queue_redraw()


func get_unresolved_asset_count() -> int:
	return _preview.get_unresolved_asset_count() if _preview != null else 0


func _ensure_preview() -> void:
	if _preview != null:
		return
	_preview = NovaMnuMenu.new()
	_preview.name = "Preview"
	_preview.build_on_ready = false
	_preview.set_edit_mode(true)
	# The canvas owns input (selection + gestures); the preview never does.
	_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_preview)


func _rebuild_preview() -> void:
	if _document == null:
		return
	_ensure_preview()
	_preview.set_resource_root(_resource_root)
	_preview.set_text_resource(_text_resource)
	# Assigning the menu rebuilds the widget tree when the preview is in the tree;
	# in edit_mode build() shows every screen, so re-apply single-screen visibility.
	_preview.menu = _document
	if _preview.is_inside_tree():
		_apply_screen_visibility()


func _apply_screen_visibility() -> void:
	if _preview == null:
		return
	var target := _visible_screen_name
	if target.is_empty() and _document != null:
		var ids := _document.get_screen_ids()
		if ids.size() > 0:
			target = _document.get_screen_name(ids[0])
	for child in _preview.get_children():
		if child is NovaMnuScreen:
			child.visible = child.get_screen_name() == target


func _on_resized() -> void:
	_recompute_fit()
	queue_redraw()


func _recompute_fit() -> void:
	if _menu_size.x <= 0.0 or _menu_size.y <= 0.0:
		return
	if size.x <= 1.0 or size.y <= 1.0:
		return
	_fit_scale = minf(size.x / _menu_size.x, size.y / _menu_size.y)
	_fit_scale = maxf(_fit_scale, 0.01)
	_fit_offset = (size - _menu_size * _fit_scale) * 0.5
	if _preview != null:
		_preview.position = _fit_offset
		_preview.scale = Vector2(_fit_scale, _fit_scale)


# --- Coordinate helpers ---------------------------------------------------------

func _canvas_to_board(p: Vector2) -> Vector2:
	return (p - _fit_offset) / _fit_scale


func _board_to_canvas(p: Vector2) -> Vector2:
	return _fit_offset + p * _fit_scale


# Sum of the local positions of every ancestor WINDOW (stopping below the screen
# container, which sits at the board origin) — the offset that turns a widget's
# parent-relative rect into an absolute board rect.
func _abs_offset_of(id: int) -> Vector2:
	var offset := Vector2.ZERO
	if _document == null:
		return offset
	var parent := _document.get_parent_id(id)
	while parent > 0 and _document.widget_exists(parent) and not _document.is_screen(parent):
		offset += _document.get_window_rect(parent).position
		parent = _document.get_parent_id(parent)
	return offset


func _abs_rect_of(id: int) -> Rect2:
	if _document == null or id < 0 or not _document.widget_exists(id) or _document.is_screen(id):
		return Rect2()
	var local := _document.get_window_rect(id)
	return Rect2(_abs_offset_of(id) + local.position, local.size)


func _visible_screen_id() -> int:
	if _document == null:
		return -1
	for sid in _document.get_screen_ids():
		if _document.get_screen_name(sid) == _visible_screen_name:
			return sid
	var ids := _document.get_screen_ids()
	return ids[0] if ids.size() > 0 else -1


# --- Picking --------------------------------------------------------------------

# Return the widget id under a canvas point: the topmost (last in pre-order, i.e.
# drawn on top) widget in the visible screen whose absolute rect contains the
# point. When nothing matches, return the screen id (the editor reads that as
# "clear to screen"). -1 only when there is no document/screen.
func pick_widget_at(canvas_point: Vector2) -> int:
	if _document == null:
		return -1
	var screen_id := _visible_screen_id()
	if screen_id < 0:
		return -1
	var board := _canvas_to_board(canvas_point)
	var root := _document.get_screen_root_id(screen_id)
	if root < 0:
		return screen_id
	var best := _pick_walk(root, board, -1)
	return best if best >= 0 else screen_id


func _pick_walk(id: int, board: Vector2, best: int) -> int:
	var r := _abs_rect_of(id)
	if r.size.x > 0.0 and r.size.y > 0.0 and r.has_point(board):
		best = id
	for child in _document.get_child_ids(id):
		best = _pick_walk(child, board, best)
	return best


# --- Resize handles -------------------------------------------------------------

# Eight handle centers in canvas space for the selected widget's absolute rect,
# ordered 0..7 (TL,TR,BL,BR,T,B,L,R). Empty when nothing resizable is selected.
func _handle_centers() -> Array:
	if not _has_resizable_selection():
		return []
	var r := _abs_rect_of(_selected_id)
	var l := r.position.x
	var t := r.position.y
	var rr := r.position.x + r.size.x
	var bb := r.position.y + r.size.y
	var cx := (l + rr) * 0.5
	var cy := (t + bb) * 0.5
	var pts := [
		Vector2(l, t), Vector2(rr, t), Vector2(l, bb), Vector2(rr, bb),
		Vector2(cx, t), Vector2(cx, bb), Vector2(l, cy), Vector2(rr, cy),
	]
	var out := []
	for p in pts:
		out.append(_board_to_canvas(p))
	return out


func _hit_handle(canvas_point: Vector2) -> int:
	var centers := _handle_centers()
	for i in range(centers.size()):
		if canvas_point.distance_to(centers[i]) <= HANDLE_HIT:
			return i
	return -1


func _has_resizable_selection() -> bool:
	if _document == null or _selected_id < 0:
		return false
	if not _document.widget_exists(_selected_id) or _document.is_screen(_selected_id):
		return false
	var r := _document.get_window_rect(_selected_id)
	return r.size.x > 0.0 and r.size.y > 0.0


# Apply a board-space drag delta to one corner/edge handle, clamping so the moving
# edge never crosses the fixed one closer than MIN_WIDGET_SIZE (no inversion).
func _apply_handle_delta(start: Rect2, handle: int, delta: Vector2) -> Rect2:
	var l := start.position.x
	var t := start.position.y
	var r := start.position.x + start.size.x
	var b := start.position.y + start.size.y
	if handle in _HANDLES_LEFT:
		l += delta.x
	if handle in _HANDLES_RIGHT:
		r += delta.x
	if handle in _HANDLES_TOP:
		t += delta.y
	if handle in _HANDLES_BOTTOM:
		b += delta.y
	if handle in _HANDLES_LEFT and l > r - MIN_WIDGET_SIZE:
		l = r - MIN_WIDGET_SIZE
	if handle in _HANDLES_RIGHT and r < l + MIN_WIDGET_SIZE:
		r = l + MIN_WIDGET_SIZE
	if handle in _HANDLES_TOP and t > b - MIN_WIDGET_SIZE:
		t = b - MIN_WIDGET_SIZE
	if handle in _HANDLES_BOTTOM and b < t + MIN_WIDGET_SIZE:
		b = t + MIN_WIDGET_SIZE
	return Rect2(l, t, r - l, b - t)


# --- Snapping (pure: no node state, unit-testable) ------------------------------

static func _snap_to_grid(v: float) -> float:
	return round(v / SNAP_GRID) * SNAP_GRID


# Snap a single moving edge: prefer a board edge (0 / extent) within tolerance,
# else the grid.
static func _snap_edge(v: float, extent: float) -> float:
	if absf(v) <= SNAP_EDGE_TOL:
		return 0.0
	if absf(v - extent) <= SNAP_EDGE_TOL:
		return extent
	return _snap_to_grid(v)


# Snap a moved position on one axis: align the near edge to 0, the far edge to the
# board extent, else snap the position to the grid (size preserved).
static func _snap_move_axis(pos: float, span: float, extent: float) -> float:
	if absf(pos) <= SNAP_EDGE_TOL:
		return 0.0
	if absf(pos + span - extent) <= SNAP_EDGE_TOL:
		return extent - span
	return _snap_to_grid(pos)


static func _snap_rect(rect: Rect2, gesture: int, handle: int, menu_size: Vector2, snap_on: bool) -> Rect2:
	if not snap_on:
		return rect
	if gesture == Gesture.MOVE:
		var nx := _snap_move_axis(rect.position.x, rect.size.x, menu_size.x)
		var ny := _snap_move_axis(rect.position.y, rect.size.y, menu_size.y)
		return Rect2(nx, ny, rect.size.x, rect.size.y)
	# RESIZE: snap only the moved edges.
	var l := rect.position.x
	var t := rect.position.y
	var r := rect.position.x + rect.size.x
	var b := rect.position.y + rect.size.y
	if handle in _HANDLES_LEFT:
		l = _snap_edge(l, menu_size.x)
	if handle in _HANDLES_RIGHT:
		r = _snap_edge(r, menu_size.x)
	if handle in _HANDLES_TOP:
		t = _snap_edge(t, menu_size.y)
	if handle in _HANDLES_BOTTOM:
		b = _snap_edge(b, menu_size.y)
	# Re-clamp the moved edges: an edge snap (tolerance > MIN_WIDGET_SIZE) can push
	# an edge across the fixed one, which would invert the rect to a negative size.
	if handle in _HANDLES_LEFT and l > r - MIN_WIDGET_SIZE:
		l = r - MIN_WIDGET_SIZE
	if handle in _HANDLES_RIGHT and r < l + MIN_WIDGET_SIZE:
		r = l + MIN_WIDGET_SIZE
	if handle in _HANDLES_TOP and t > b - MIN_WIDGET_SIZE:
		t = b - MIN_WIDGET_SIZE
	if handle in _HANDLES_BOTTOM and b < t + MIN_WIDGET_SIZE:
		b = t + MIN_WIDGET_SIZE
	return Rect2(l, t, r - l, b - t)


# --- Input ----------------------------------------------------------------------

# The preview is MOUSE_FILTER_IGNORE and this canvas is MOUSE_FILTER_STOP, so the
# canvas owns every mouse event over the board (the preview never competes).
func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index != MOUSE_BUTTON_LEFT:
			return
		if mb.pressed:
			_on_press(mb.position)
		else:
			_on_release()
		accept_event()
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _pressed:
			_on_drag(mm.position, mm.alt_pressed)
			accept_event()
		else:
			_update_cursor(mm.position)


func _on_press(pos: Vector2) -> void:
	grab_focus()
	# A grab on a resize handle of the current selection takes priority over a pick.
	if _has_resizable_selection():
		var h := _hit_handle(pos)
		if h >= 0:
			_active_handle = h
			_gesture = Gesture.RESIZE
			_begin_drag(pos)
			return
	var picked := pick_widget_at(pos)
	if picked < 0 or (_document != null and _document.is_screen(picked)):
		# Empty board: clear selection (the editor selects the screen).
		_selected_id = -1
		_reset_gesture()
		queue_redraw()
		selection_cleared.emit()
		return
	if picked != _selected_id:
		_selected_id = picked
		queue_redraw()
		widget_picked.emit(picked)
	_active_handle = -1
	_gesture = Gesture.MOVE
	_begin_drag(pos)


func _begin_drag(pos: Vector2) -> void:
	_press_pos = pos
	_drag_start_rect = _abs_rect_of(_selected_id)
	_drag_rect = _drag_start_rect
	_pressed = true
	_passed_threshold = false


func _on_drag(pos: Vector2, alt: bool) -> void:
	if _gesture == Gesture.NONE:
		return
	if not _passed_threshold:
		if pos.distance_to(_press_pos) < DRAG_THRESHOLD:
			return
		_passed_threshold = true
	var board_delta := (pos - _press_pos) / _fit_scale
	var new_rect: Rect2
	if _gesture == Gesture.RESIZE:
		new_rect = _apply_handle_delta(_drag_start_rect, _active_handle, board_delta)
	else:
		new_rect = Rect2(_drag_start_rect.position + board_delta, _drag_start_rect.size)
	_drag_rect = _snap_rect(new_rect, _gesture, _active_handle, _menu_size, not alt)
	queue_redraw()


func _on_release() -> void:
	if _pressed and _passed_threshold and _has_selection_widget():
		# Convert the ghost (absolute) back to a parent-relative rect and commit it
		# as one edit; the editor's apply_edit no-ops a zero-delta gesture.
		var local := Rect2(_drag_rect.position - _abs_offset_of(_selected_id), _drag_rect.size)
		rect_committed.emit(_selected_id, local)
	_reset_gesture()
	queue_redraw()


func _has_selection_widget() -> bool:
	return _document != null and _selected_id >= 0 \
		and _document.widget_exists(_selected_id) and not _document.is_screen(_selected_id)


func _reset_gesture() -> void:
	_gesture = Gesture.NONE
	_active_handle = -1
	_pressed = false
	_passed_threshold = false
	_drag_rect = Rect2()


func _update_cursor(pos: Vector2) -> void:
	var shape := Control.CURSOR_ARROW
	var h := _hit_handle(pos)
	if h >= 0:
		match h:
			0, 3: shape = Control.CURSOR_FDIAGSIZE
			1, 2: shape = Control.CURSOR_BDIAGSIZE
			4, 5: shape = Control.CURSOR_VSIZE
			6, 7: shape = Control.CURSOR_HSIZE
	elif _has_selection_widget() and _abs_rect_of(_selected_id).has_point(_canvas_to_board(pos)):
		shape = Control.CURSOR_MOVE
	mouse_default_cursor_shape = shape


# Arrow keys nudge the selected widget by 1 board unit (Shift = grid step). Lives
# here (not the editor's undo shortcut) so it reuses the rect_committed commit path
# and defers to a focused spinner/tree that wants the arrows first: an unhandled
# arrow only reaches here when nothing else consumed it. A focused text field still
# guards explicitly (LineEdit ignores Up/Down, which would otherwise leak through).
func _unhandled_key_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return
	if not _has_selection_widget():
		return
	var focus := get_viewport().gui_get_focus_owner()
	if focus is LineEdit or focus is TextEdit or focus is SpinBox:
		return
	var step := SNAP_GRID if key.shift_pressed else 1.0
	var d := Vector2.ZERO
	match key.keycode:
		KEY_LEFT: d = Vector2(-step, 0.0)
		KEY_RIGHT: d = Vector2(step, 0.0)
		KEY_UP: d = Vector2(0.0, -step)
		KEY_DOWN: d = Vector2(0.0, step)
		_: return
	var local := _document.get_window_rect(_selected_id)
	rect_committed.emit(_selected_id, Rect2(local.position + d, local.size))
	get_viewport().set_input_as_handled()


# --- Draw -----------------------------------------------------------------------

func _draw() -> void:
	_recompute_fit()
	draw_rect(Rect2(Vector2.ZERO, size), COL_BG)
	var board := Rect2(_fit_offset, _menu_size * _fit_scale)
	draw_rect(board, COL_LETTERBOX)
	draw_rect(board, COL_BORDER, false, 1.0)

	# During a drag, draw the live ghost instead of the static selection.
	if _gesture != Gesture.NONE and _passed_threshold:
		var ghost := Rect2(_board_to_canvas(_drag_rect.position), _drag_rect.size * _fit_scale)
		draw_rect(ghost, COL_GHOST, false, 2.0)
		return

	var sel := _abs_rect_of(_selected_id)
	if sel.size.x > 0.0 and sel.size.y > 0.0:
		var screen_rect := Rect2(_board_to_canvas(sel.position), sel.size * _fit_scale)
		draw_rect(screen_rect, COL_SELECT, false, 2.0)
		var half := HANDLE_SIZE * 0.5
		for c in _handle_centers():
			draw_rect(Rect2(c - Vector2(half, half), Vector2(HANDLE_SIZE, HANDLE_SIZE)), COL_HANDLE)
