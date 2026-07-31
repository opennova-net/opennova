class_name MnuCanvas
extends Control

# WYSIWYG edit surface for the Menus workspace. Owns a live NovaMnuMenu in
# edit_mode (inert: no navigation, audio, or cursor side effects) scaled to the
# fixed 800x600 design space all Joint Operations (JO) and newer menus are authored
# in. The fit is anamorphic (independent X/Y factors, no letterbox bars), matching
# the runtime so the canvas previews exactly what the game draws [orig:
# CUIScene_SetScreenScale @ 0x639480].
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
const COL_OFFBOARD := Color(0.90, 0.30, 0.25, 0.95) # selection/ghost outline when off the board
const COL_GHOST := Color(0.45, 0.78, 1.0, 0.95)
const COL_HANDLE := Color(0.95, 0.95, 0.97, 1.0)
const COL_HOVER := Color(0.55, 0.85, 1.0, 0.6)      # widget under the cursor (pre-click)
const COL_ALLBOUNDS := Color(1, 1, 1, 0.12)         # faint outline for every widget
const COL_LABEL_BG := Color(0.07, 0.08, 0.09, 0.85) # selection-caption backing
const COL_MARQUEE_FILL := Color(0.55, 0.85, 1.0, 0.08) # rubber-band interior

# Gesture tuning (canvas pixels unless noted). Board units are the document's
# authoring coordinates (the doc stores ints, so integer snapping is implicit).
const HANDLE_SIZE := 8.0       # drawn handle square side (canvas px)
const HANDLE_HIT := 10.0       # handle grab tolerance (canvas px)
const DRAG_THRESHOLD := 4.0    # px of motion before a press becomes a drag
const CYCLE_TOL := 4.0         # px: clicks within this of the last pick cycle the z-stack
const MIN_WIDGET_SIZE := 4.0   # smallest resize extent (board units)
const SNAP_GRID := 8.0         # snap grid step (board units)
const SNAP_EDGE_TOL := 6.0     # snap-to-board-edge tolerance (board units)
const ZOOM_MIN := 0.1          # zoom-out floor
const ZOOM_MAX := 8.0          # zoom-in ceiling
const ZOOM_STEP := 1.1         # per wheel-notch zoom factor
const PAN_MARGIN := 40.0       # px of board kept on-canvas when panning

enum Gesture { NONE, MOVE, RESIZE, MARQUEE }

# Handle order is fixed (indices used by hit-test, draw, snap, and cursor):
# 0 TL, 1 TR, 2 BL, 3 BR (corners), 4 T, 5 B, 6 L, 7 R (edge midpoints).
const _HANDLES_LEFT := [0, 2, 6]
const _HANDLES_RIGHT := [1, 3, 7]
const _HANDLES_TOP := [0, 1, 4]
const _HANDLES_BOTTOM := [2, 3, 5]

signal widget_picked(id: int)
signal selection_cleared()
signal rect_committed(id: int, local_rect: Rect2)
# Multi-select: the full selection changed via a canvas gesture (shift/ctrl toggle
# or marquee). A rigid group-move commits every member's new parent-relative rect
# in one batch (the editor folds it into a single undo step).
signal selection_set(ids: PackedInt32Array)
signal rect_committed_batch(edits: Array)

var _preview: NovaMnuMenu
var _document: NovaMnuDocument
var _resource_root: NovaResourceRoot
var _text_resource: RtxtStringFile
var _stylesheet: MnsStyleSheet
# The document's own .mnu basename (shipped self-file screen actions compare
# against it; see NovaMnuMenu.set_menu_file).
var _menu_file := ""

# The fixed 800x600 design space all JO+ menus author in (the engine scales it to
# the screen anamorphically; we do the same here) [orig: CUIScene_SetScreenScale
# @ 0x639480].
var _menu_size := Vector2(800, 600)
# _fit_scale / _fit_offset are the base anamorphic fit (board -> canvas), with
# independent X/Y scale factors. _zoom (scalar) and _pan layer on top so the user can
# magnify small widgets and scroll around; the effective transform is _eff_scale() /
# _eff_offset(), which everything routes through (so picking, handles, snapping, and
# the live preview all stay aligned).
var _fit_scale := Vector2.ONE
var _fit_offset := Vector2.ZERO
var _zoom := 1.0
var _pan := Vector2.ZERO
var _visible_screen_name := ""

# Interactive "play" preview: the live menu drives its own navigation (click a tab
# and only its window shows). Author gestures (select/drag/marquee/pick) and the
# authoring overlays are suppressed; only view gestures (pan/zoom) stay live.
var _interactive := false

# View-gesture state (pan via middle-drag or Space+left-drag).
var _panning := false
var _pan_last := Vector2.ZERO
var _space_held := false

# Selection + live gesture state. _selected_id is the ACTIVE widget (last clicked;
# drives the resize handles, arrow nudge, caption, and single-widget inspector).
# _selection is the full set (shift/ctrl-click or marquee); when its size is <= 1
# every path behaves identically to single-select. Both are kept in sync with the
# editor (set_selected / set_selection). _drag_rect is the ghost in board space.
var _selected_id := -1
var _selection: PackedInt32Array = PackedInt32Array()
var _gesture: int = Gesture.NONE
var _active_handle := -1
var _pressed := false
var _passed_threshold := false
var _press_pos := Vector2.ZERO
var _drag_start_rect := Rect2()
var _drag_rect := Rect2()

# Multi-select gesture state. _group_start_rects maps each selected id to its
# absolute rect at press; _group_delta is the snapped board delta applied to the
# whole set during a rigid move. _marquee_to is the live rubber-band corner (canvas
# space). _pending_collapse_id collapses a multi-selection down to one widget when a
# press on a member is released without a drag.
var _group_start_rects := {}
var _group_delta := Vector2.ZERO
var _marquee_to := Vector2.ZERO
var _pending_collapse_id := -1
# What a marquee press landed on, so a release WITHOUT a drag restores the old click
# behavior: select the background root window, or clear when it was truly empty.
var _marquee_click_target := -1

# Picking aids. _hover_id is the widget under the cursor between gestures (a hover
# outline so the user sees what a click will select). _show_all_bounds draws a faint
# outline for every widget so tiny / empty / overlapping widgets are locatable.
var _hover_id := -1
var _show_all_bounds := true

# Right-click "select under cursor" menu: lists every widget in the hit-stack
# (depth-indented) so a nested / obscured widget is one click away, without the
# invisible z-cycling. Built lazily and reused.
var _pick_menu: PopupMenu

# Maps a widget's stable document id to its live preview Control (tagged by the
# builder with the "mnu_widget_id" meta). Rebuilt with the preview. Shipped menus
# often omit a widget's RIGHT/BOTTOM, so the document rect is sizeless; the live
# Control carries the real rendered size (texture / type default) the overlay needs
# to pick + outline it. Positions are always authored, so only size is sourced here.
var _id_to_control: Dictionary = {}

# Z-cycle: successive clicks within CYCLE_TOL of the same point step through the
# stack of overlapping widgets (top -> bottom) so an obscured widget is reachable.
var _cycle_stack: PackedInt32Array = PackedInt32Array()
var _cycle_index := 0
var _cycle_anchor := Vector2(-9999, -9999)


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	focus_mode = Control.FOCUS_CLICK
	if not resized.is_connected(_on_resized):
		resized.connect(_on_resized)
	if not mouse_exited.is_connected(_on_mouse_exited):
		mouse_exited.connect(_on_mouse_exited)
	_rebuild_preview()
	_recompute_fit()


# Point the preview at a document. resource_root/text_resource are optional; when
# absent the builder degrades to placeholder visuals (M3 behavior). menu_file is
# the document's own .mnu basename (refreshed on every rebind; a Save As while
# the preview plays keeps the old name until the next rebind).
func set_menu(doc: NovaMnuDocument, resource_root: NovaResourceRoot, text_resource: RtxtStringFile, stylesheet: MnsStyleSheet = null, menu_file := "") -> void:
	_document = doc
	_resource_root = resource_root
	_text_resource = text_resource
	_stylesheet = stylesheet
	_menu_file = menu_file
	if doc != null:
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


func is_interactive() -> bool:
	return _interactive


## Observable live-preview state for editor tools and tests. Callers never need
## to reach through the canvas into the id-to-Control implementation map.
func get_preview_widget_state(id: int) -> MnuPreviewWidgetState:
	var state := MnuPreviewWidgetState.new()
	var control: Variant = _id_to_control.get(id)
	if control == null or not is_instance_valid(control):
		return state
	state.exists = true
	state.visible = control.is_visible_in_tree()
	state.pressable = control is BaseButton or control is NovaMnuEdit \
		or control is NovaMnuGoto
	if control is BaseButton:
		var button := control as BaseButton
		state.disabled = button.disabled
		state.pressed = button.button_pressed
	return state


## Activate one live preview widget with click/hotkey semantics. The canvas owns
## the runtime-Control mapping, so activation stays behind the same small seam
## as preview-state queries.
func activate_preview_widget(id: int) -> MnuPreviewWidgetState:
	var state := get_preview_widget_state(id)
	if not state.exists or not state.visible or not state.pressable:
		return state
	var control: Variant = _id_to_control.get(id)
	if control is BaseButton:
		var button := control as BaseButton
		if button.disabled:
			return state
		if button.toggle_mode:
			button.set_pressed(not button.button_pressed)
		button.emit_signal(&"pressed")
	elif control is NovaMnuEdit:
		(control as NovaMnuEdit).trigger_hotkey()
	elif control is NovaMnuGoto:
		(control as NovaMnuGoto).trigger()
	state = get_preview_widget_state(id)
	state.activated = true
	return state


## The authored Rect may omit width/height for auto-sized controls. Layout tools
## consume the rendered extent through this query rather than canvas internals.
func get_rendered_widget_rect(id: int) -> Rect2:
	return _abs_rect_of(id) if _is_widget(id) else Rect2()


# Toggle the interactive "play" preview. On: the live menu's navigators wire up and
# clicking a tab runs its window show/hide actions (the preview rebuilds, so authored
# window visibility resets and only the current screen shows). Off: returns to the
# authoring canvas (single-screen view, inert widgets) with the prior selection intact.
func set_interactive(on: bool) -> void:
	if _interactive == on or _preview == null:
		return
	_interactive = on
	if on:
		# Start the sandbox on the screen the author is viewing, then go live.
		_preview.set_current_screen(_visible_screen_name)
		_preview.set_interactive(true)
		if not _preview.screen_changed.is_connected(_on_preview_screen_changed):
			_preview.screen_changed.connect(_on_preview_screen_changed)
	else:
		if _preview.screen_changed.is_connected(_on_preview_screen_changed):
			_preview.screen_changed.disconnect(_on_preview_screen_changed)
		_preview.set_interactive(false)
		_apply_screen_visibility()  # C++ build() shows all screens; restore single-screen author view
	_rebuild_control_map()
	queue_redraw()


# While interactive, follow the live menu's own screen navigation so leaving the mode
# returns the author to whatever screen they ended on.
func _on_preview_screen_changed(screen_name: String) -> void:
	_visible_screen_name = screen_name


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
# Collapses any prior multi-selection to this single id.
func set_selected(id: int) -> void:
	_selected_id = id
	_selection = PackedInt32Array([id]) if _is_widget(id) else PackedInt32Array()
	queue_redraw()


# Set the full multi-selection (programmatic, from the editor: select_widgets,
# batch-move re-select). The active id is the last member (drives handles + caption);
# an empty set clears. Never re-emits (programmatic).
func set_selection(ids: PackedInt32Array) -> void:
	_selection = ids
	_selected_id = ids[ids.size() - 1] if ids.size() > 0 else -1
	queue_redraw()


# True when id is a concrete, selectable widget (a real widget, not a screen).
func _is_widget(id: int) -> bool:
	return _document != null and id >= 0 and _document.widget_exists(id) and not _document.is_screen(id)


func get_unresolved_asset_count() -> int:
	return _preview.get_unresolved_asset_count() if _preview != null else 0


# Toggle the faint outline drawn around every widget (the editor toolbar drives this).
func set_show_all_bounds(on: bool) -> void:
	if _show_all_bounds == on:
		return
	_show_all_bounds = on
	queue_redraw()


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
	_preview.set_stylesheet(_stylesheet)
	_preview.set_menu_file(_menu_file)
	# Assigning the menu rebuilds the widget tree when the preview is in the tree;
	# in edit_mode build() shows every screen, so re-apply single-screen visibility.
	_preview.menu = _document
	if _preview.is_inside_tree():
		_apply_screen_visibility()
	_rebuild_control_map()


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
	# Anamorphic fill (matches the runtime): the 800x600 board stretches to fill the
	# canvas with independent X/Y factors, no letterbox bars [orig:
	# CUIScene_SetScreenScale @ 0x639480]. Zoom + pan layer on top for authoring.
	_fit_scale = Vector2(maxf(size.x / _menu_size.x, 0.01), maxf(size.y / _menu_size.y, 0.01))
	_fit_offset = Vector2.ZERO
	_clamp_pan()
	if _preview != null:
		_preview.position = _eff_offset()
		_preview.scale = _eff_scale()


# --- Coordinate helpers ---------------------------------------------------------

# The effective board -> canvas transform = base letterbox fit composed with the
# user's zoom + pan. Everything (picking, draw, drag deltas, the preview node)
# routes through these two so the overlay never drifts from the rendered menu.
func _eff_scale() -> Vector2:
	return _fit_scale * _zoom


func _eff_offset() -> Vector2:
	return _fit_offset + _pan


func _canvas_to_board(p: Vector2) -> Vector2:
	return (p - _eff_offset()) / _eff_scale()


func _board_to_canvas(p: Vector2) -> Vector2:
	return _eff_offset() + p * _eff_scale()


# Keep at least PAN_MARGIN px of the board on-canvas so it can never be lost, while
# still allowing pan into negative / off-board coordinates to inspect overhang.
func _clamp_pan() -> void:
	if size.x <= 1.0 or size.y <= 1.0:
		return
	var board_size := _menu_size * _eff_scale()
	var off := _fit_offset + _pan
	off.x = clampf(off.x, PAN_MARGIN - board_size.x, size.x - PAN_MARGIN)
	off.y = clampf(off.y, PAN_MARGIN - board_size.y, size.y - PAN_MARGIN)
	_pan = off - _fit_offset


# Zoom toward a canvas point, keeping the board point under it fixed. Clamped.
func _zoom_at(canvas_point: Vector2, factor: float) -> void:
	var new_zoom := clampf(_zoom * factor, ZOOM_MIN, ZOOM_MAX)
	if is_equal_approx(new_zoom, _zoom):
		return
	var board := _canvas_to_board(canvas_point)  # board point under the cursor, pre-zoom
	_pan = _pan_for_zoom_at(_fit_scale, _fit_offset, board, canvas_point, new_zoom)
	_zoom = new_zoom
	_recompute_fit()  # re-applies preview scale/position + re-clamps pan
	queue_redraw()


# Pure: the pan that makes board_pt map back to canvas_pt at new_zoom. Unit-testable.
# fit_scale is the anamorphic (per-axis) base fit; the zoom multiplies it uniformly.
static func _pan_for_zoom_at(fit_scale: Vector2, fit_offset: Vector2, board_pt: Vector2, canvas_pt: Vector2, new_zoom: float) -> Vector2:
	return canvas_pt - fit_offset - board_pt * (fit_scale * new_zoom)


# Reset to the plain letterbox fit (the toolbar Fit button + a document open).
func reset_view() -> void:
	_zoom = 1.0
	_pan = Vector2.ZERO
	_recompute_fit()
	queue_redraw()


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
	# Position is always authored; a missing RIGHT/BOTTOM leaves that size axis 0. Fill
	# only the missing axis from the live Control's rendered size (doc size wins when
	# present), so a sizeless-but-rendered widget (e.g. a shipped button) is pickable
	# and outlined where it actually draws.
	var sz := local.size
	if sz.x <= 0.0 or sz.y <= 0.0:
		var live := _live_size_of(id)
		if sz.x <= 0.0 and live.x > 0.0:
			sz.x = live.x
		if sz.y <= 0.0 and live.y > 0.0:
			sz.y = live.y
	return Rect2(_abs_offset_of(id) + local.position, sz)


# The rendered size (board units) of a widget's live preview Control, or zero when it
# has none (no preview, or a widget added after the last build). Control.size is the
# unscaled local size; only the preview root carries _eff_scale, so it is already in
# board units.
func _live_size_of(id: int) -> Vector2:
	var c = _id_to_control.get(id)
	if c != null and is_instance_valid(c):
		return (c as Control).size
	return Vector2.ZERO


# Rebuild the id -> live Control map by walking the preview for builder-tagged nodes.
# Called whenever the preview rebuilds; entries are validated on read.
func _rebuild_control_map() -> void:
	_id_to_control.clear()
	if _preview != null:
		_index_controls(_preview)


func _index_controls(node: Node) -> void:
	if node.has_meta("mnu_widget_id"):
		_id_to_control[int(node.get_meta("mnu_widget_id"))] = node
	for c in node.get_children():
		_index_controls(c)


func _visible_screen_id() -> int:
	if _document == null:
		return -1
	for sid in _document.get_screen_ids():
		if _document.get_screen_name(sid) == _visible_screen_name:
			return sid
	var ids := _document.get_screen_ids()
	return ids[0] if ids.size() > 0 else -1


# --- Picking --------------------------------------------------------------------

# Return the widget id under a canvas point: the topmost (drawn on top) widget in
# the visible screen whose absolute rect contains the point. When nothing matches,
# return the screen id (the editor reads that as "clear to screen"). -1 only when
# there is no document/screen.
func pick_widget_at(canvas_point: Vector2) -> int:
	var stack := pick_stack_at(canvas_point)
	return stack[0] if not stack.is_empty() else _visible_screen_id()


# Every widget under the point, ordered top -> bottom (index 0 is what a plain click
# selects). Pre-order collects parent-before-children; reversing puts the deepest /
# last-drawn first, matching the old single-pick result for stack[0].
func pick_stack_at(canvas_point: Vector2) -> PackedInt32Array:
	var out := PackedInt32Array()
	if _document == null:
		return out
	var screen_id := _visible_screen_id()
	if screen_id < 0:
		return out
	var board := _canvas_to_board(canvas_point)
	var root := _document.get_screen_root_id(screen_id)
	if root < 0:
		return out
	_pick_collect(root, board, out)
	out.reverse()
	return out


func _pick_collect(id: int, board: Vector2, out: PackedInt32Array) -> void:
	var r := _abs_rect_of(id)
	if r.size.x > 0.0 and r.size.y > 0.0 and r.has_point(board):
		out.append(id)
	for child in _document.get_child_ids(id):
		_pick_collect(child, board, out)


# Pick under the cursor, cycling through the overlapping stack on repeated clicks at
# ~the same point so an obscured widget can be reached. A click that moves beyond
# CYCLE_TOL, or hits a different stack, restarts at the top. Not reset on the
# editor's selection echo (set_selected), so same-spot clicks keep advancing.
func _pick_with_cycle(canvas_point: Vector2) -> int:
	var stack := pick_stack_at(canvas_point)
	if stack.is_empty():
		_cycle_stack = stack
		_cycle_index = 0
		_cycle_anchor = canvas_point
		return _visible_screen_id()
	if canvas_point.distance_to(_cycle_anchor) <= CYCLE_TOL and stack == _cycle_stack:
		_cycle_index = (_cycle_index + 1) % stack.size()
	else:
		_cycle_stack = stack
		_cycle_index = 0
		_cycle_anchor = canvas_point
	return stack[_cycle_index]


# Every non-screen widget id in the visible screen (for the show-all-bounds overlay).
func _visible_widget_ids() -> PackedInt32Array:
	var out := PackedInt32Array()
	var screen_id := _visible_screen_id()
	if _document == null or screen_id < 0:
		return out
	var root := _document.get_screen_root_id(screen_id)
	if root >= 0:
		_walk_ids(_document, root, out)
	return out


# Pre-order collect of every non-screen widget id under a subtree. Pure/static so it
# is unit-testable like the snapping helpers.
static func _walk_ids(doc: NovaMnuDocument, id: int, out: PackedInt32Array) -> void:
	if doc == null or id < 0 or not doc.widget_exists(id):
		return
	if not doc.is_screen(id):
		out.append(id)
	for child in doc.get_child_ids(id):
		_walk_ids(doc, child, out)


# Every selectable widget UNDER the screen root window whose absolute rect (via
# _abs_rect_of, so live-aware for sizeless-but-rendered widgets) intersects board_rect
# (a board-space marquee). The root window itself is the screen background, not a
# marquee target, so collection starts at its children; screens are excluded by
# _abs_rect_of returning an empty rect.
func _collect_marquee(id: int, board_rect: Rect2, out: PackedInt32Array) -> void:
	if _document == null or not _document.widget_exists(id):
		return
	var my := _abs_rect_of(id)
	if my.size.x > 0.0 and my.size.y > 0.0 and board_rect.intersects(my):
		out.append(id)
	for child in _document.get_child_ids(id):
		_collect_marquee(child, board_rect, out)


# Build the right-click "select under cursor" menu and pop it up. The stack is every
# widget the point hits (top -> bottom); each is one click away, so a nested or
# obscured widget needs no z-cycling. The chosen id routes through the normal pick
# path (local state + widget_picked) so the editor syncs the tree + inspector.
func _show_pick_menu(pos: Vector2) -> void:
	if _document == null:
		return
	var stack := pick_stack_at(pos)
	if stack.is_empty():
		return
	_ensure_pick_menu()
	_pick_menu.clear()
	var rows := _pick_menu_rows(_document, stack)
	for i in range(rows.size()):
		var row: Dictionary = rows[i]
		_pick_menu.add_item("    ".repeat(int(row["depth"])) + String(row["label"]), i)
		_pick_menu.set_item_metadata(i, int(row["id"]))
	if _pick_menu.item_count == 0:
		return
	_pick_menu.reset_size()
	_pick_menu.position = Vector2i(get_screen_position() + pos)
	_pick_menu.popup()


func _ensure_pick_menu() -> void:
	if _pick_menu != null:
		return
	_pick_menu = PopupMenu.new()
	_pick_menu.name = "PickMenu"
	add_child(_pick_menu)
	_pick_menu.index_pressed.connect(_on_pick_menu_index_pressed)


func _on_pick_menu_index_pressed(index: int) -> void:
	if _pick_menu == null:
		return
	var id := int(_pick_menu.get_item_metadata(index))
	if not _is_widget(id):
		return
	# Mirror a plain pick: replace the selection locally, then announce it so the
	# editor mirrors it to the tree + inspector (see _on_press).
	_selected_id = id
	_selection = PackedInt32Array([id])
	queue_redraw()
	widget_picked.emit(id)


# Pure/static: one menu row per stacked id (same order as pick_stack_at: topmost
# first), each with a caption-style label and its nesting depth (ancestor windows
# below the screen) for an indent. Unit-testable like _walk_ids.
static func _pick_menu_rows(doc: NovaMnuDocument, stack: PackedInt32Array) -> Array:
	var out := []
	if doc == null:
		return out
	for id in stack:
		if not doc.widget_exists(id) or doc.is_screen(id):
			continue
		var type_name := doc.get_widget_type_name(doc.get_widget_type(id))
		var wname := doc.get_widget_name(id)
		var label := "(%s)  #%d" % [type_name, id] if wname.is_empty() else "%s (%s)  #%d" % [wname, type_name, id]
		out.append({"id": id, "label": label, "depth": _depth_below_screen(doc, id)})
	return out


# Number of ancestor windows between id and its screen container (a root window = 0).
static func _depth_below_screen(doc: NovaMnuDocument, id: int) -> int:
	var d := 0
	var p := doc.get_parent_id(id)
	while p > 0 and doc.widget_exists(p) and not doc.is_screen(p):
		d += 1
		p = doc.get_parent_id(p)
	return d


# --- Resize handles -------------------------------------------------------------

# Eight handle centers in canvas space for the selected widget's absolute rect,
# ordered 0..7 (TL,TR,BL,BR,T,B,L,R). Empty when nothing resizable is selected.
func _handle_centers() -> Array:
	# Resize is single-active only: a multi-selection shows no handles.
	if _selection.size() > 1 or not _has_resizable_selection():
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
	# Use the rendered rect (live-aware), so a widget whose document rect omits its
	# size still shows handles + is resizable (the drag then pins a concrete size).
	var r := _abs_rect_of(_selected_id)
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
		match mb.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				if mb.pressed:
					_zoom_at(mb.position, ZOOM_STEP)
				accept_event()
			MOUSE_BUTTON_WHEEL_DOWN:
				if mb.pressed:
					_zoom_at(mb.position, 1.0 / ZOOM_STEP)
				accept_event()
			MOUSE_BUTTON_MIDDLE:
				_panning = mb.pressed
				_pan_last = mb.position
				_update_cursor(mb.position)
				accept_event()
			MOUSE_BUTTON_LEFT:
				# Space+left pans instead of selecting (a common 2D-canvas convention).
				if _space_held:
					_panning = mb.pressed
					_pan_last = mb.position
					accept_event()
				elif _interactive:
					# The live menu owns clicks (they reach its buttons directly); the
					# canvas does no authoring selection while playing.
					pass
				elif mb.pressed:
					_on_press(mb.position, mb.shift_pressed or mb.ctrl_pressed)
					accept_event()
				else:
					_on_release()
					accept_event()
			MOUSE_BUTTON_RIGHT:
				# Right-click lists every widget under the cursor so a nested one is
				# directly selectable (no z-cycling). Does not start a gesture. Off
				# while interactive (no authoring).
				if not _interactive:
					if mb.pressed:
						_show_pick_menu(mb.position)
					accept_event()
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _panning:
			_pan += mm.position - _pan_last
			_pan_last = mm.position
			_recompute_fit()  # re-applies preview position + clamps pan
			queue_redraw()
			accept_event()
		elif _interactive:
			pass  # live menu handles its own hover; no authoring cursor/hover cues
		elif _pressed:
			_on_drag(mm.position, mm.alt_pressed)
			accept_event()
		else:
			_update_cursor(mm.position)
			_update_hover(mm.position)


func _on_press(pos: Vector2, additive := false) -> void:
	grab_focus()
	# A grab on a resize handle takes priority — single-active only (no group resize).
	if not additive and _selection.size() <= 1 and _has_resizable_selection():
		var h := _hit_handle(pos)
		if h >= 0:
			_active_handle = h
			_gesture = Gesture.RESIZE
			_begin_drag(pos)
			return
	# Shift/Ctrl-click toggles a widget in/out of the selection (topmost pick, no
	# z-cycle; arms no drag, so it is purely a selection-set change).
	if additive:
		var hit := pick_widget_at(pos)
		if hit >= 0 and not (_document != null and _document.is_screen(hit)):
			_toggle_member(hit)
		_reset_gesture()
		queue_redraw()
		return
	var picked := _pick_with_cycle(pos)
	if picked < 0 or (_document != null and _document.is_screen(picked)) or _is_root_window(picked):
		# Empty board or the screen background (root window): arm a marquee. A release
		# without a drag restores the old click behavior (select the root / clear).
		_arm_marquee(pos, picked)
		return
	if _selection.size() > 1 and _selection.has(picked):
		# Press on a member of a multi-selection: arm a rigid group-move. A release
		# without a drag collapses the selection down to this one widget.
		_selected_id = picked
		_pending_collapse_id = picked
		queue_redraw()
		_active_handle = -1
		_gesture = Gesture.MOVE
		_begin_group_drag(pos)
		return
	# Plain click on a single / non-member widget: replace the selection.
	if picked != _selected_id or _selection.size() != 1:
		_selected_id = picked
		_selection = PackedInt32Array([picked])
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
	_hover_id = -1  # no pre-click hover cue while a gesture is live


# Add or remove a widget from the multi-selection, then announce the new set. The
# active id becomes the toggled widget (when added) or the last remaining member
# (when removed), so the handles/caption track a sensible widget.
func _toggle_member(id: int) -> void:
	var idx := _selection.find(id)
	if idx >= 0:
		_selection.remove_at(idx)
		_selected_id = _selection[_selection.size() - 1] if _selection.size() > 0 else -1
	else:
		_selection.append(id)
		_selected_id = id
	selection_set.emit(_selection)


# Arm a rubber-band selection on an empty-board press. The gesture only becomes a
# marquee once the drag threshold is crossed; a release before then clears (handled
# in _on_release), preserving the old empty-click-clears behavior.
func _arm_marquee(pos: Vector2, click_target: int) -> void:
	_gesture = Gesture.MARQUEE
	_marquee_click_target = click_target
	_press_pos = pos
	_marquee_to = pos
	_pressed = true
	_passed_threshold = false
	_hover_id = -1


# A non-screen widget whose parent is a screen: the screen's background root window.
func _is_root_window(id: int) -> bool:
	if _document == null or not _is_widget(id):
		return false
	var p := _document.get_parent_id(id)
	return p > 0 and _document.is_screen(p)


# Begin a rigid move of the whole selection. Snapshots each member's absolute rect
# (the active widget's rect is the snap anchor); _group_delta accumulates in _on_drag.
func _begin_group_drag(pos: Vector2) -> void:
	_press_pos = pos
	_group_start_rects = {}
	for id in _selection:
		_group_start_rects[id] = _abs_rect_of(id)
	_drag_start_rect = _abs_rect_of(_selected_id)
	_drag_rect = _drag_start_rect
	_group_delta = Vector2.ZERO
	_pressed = true
	_passed_threshold = false
	_hover_id = -1


func _on_drag(pos: Vector2, alt: bool) -> void:
	if _gesture == Gesture.NONE:
		return
	if not _passed_threshold:
		if pos.distance_to(_press_pos) < DRAG_THRESHOLD:
			return
		_passed_threshold = true
		_pending_collapse_id = -1  # a real drag never collapses the selection
	if _gesture == Gesture.MARQUEE:
		_marquee_to = pos
		queue_redraw()
		return
	var board_delta := (pos - _press_pos) / _eff_scale()
	if _gesture == Gesture.MOVE and _selection.size() > 1:
		# Snap the ACTIVE widget; its snapped move is the delta applied to the set.
		var snapped := _snap_rect(Rect2(_drag_start_rect.position + board_delta, _drag_start_rect.size),
			Gesture.MOVE, -1, _menu_size, not alt)
		_group_delta = snapped.position - _drag_start_rect.position
		_drag_rect = snapped
		queue_redraw()
		return
	var new_rect: Rect2
	if _gesture == Gesture.RESIZE:
		new_rect = _apply_handle_delta(_drag_start_rect, _active_handle, board_delta)
	else:
		new_rect = Rect2(_drag_start_rect.position + board_delta, _drag_start_rect.size)
	_drag_rect = _snap_rect(new_rect, _gesture, _active_handle, _menu_size, not alt)
	queue_redraw()


func _on_release() -> void:
	if _gesture == Gesture.MARQUEE:
		if _passed_threshold:
			# Rubber-band: select every widget the box touches.
			_selection = _marquee_ids(_marquee_board_rect())
			_selected_id = _selection[_selection.size() - 1] if _selection.size() > 0 else -1
			selection_set.emit(_selection)
		elif _is_widget(_marquee_click_target):
			# Click (no drag) on the background root window: select it, as before.
			_selected_id = _marquee_click_target
			_selection = PackedInt32Array([_marquee_click_target])
			widget_picked.emit(_marquee_click_target)
		else:
			# Click on truly empty board: clear (the editor then selects the screen).
			_selection = PackedInt32Array()
			_selected_id = -1
			selection_cleared.emit()
		_reset_gesture()
		queue_redraw()
		return
	if _pressed and _passed_threshold and _gesture == Gesture.MOVE and _selection.size() > 1:
		# Rigid group-move: commit every member's new parent-relative rect as one batch.
		var edits := _build_group_edits()
		if not edits.is_empty():
			rect_committed_batch.emit(edits)
		_reset_gesture()
		queue_redraw()
		return
	if not _passed_threshold and _pending_collapse_id >= 0:
		# A click (no drag) on a member of a multi-selection collapses to that widget.
		var keep := _pending_collapse_id
		_selection = PackedInt32Array([keep])
		_selected_id = keep
		_reset_gesture()
		queue_redraw()
		widget_picked.emit(keep)
		return
	if _pressed and _passed_threshold and _has_selection_widget():
		# Single move/resize: convert the ghost (absolute) to a parent-relative rect and
		# commit it as one edit; the editor's apply_edit no-ops a zero-delta gesture.
		var local := Rect2(_drag_rect.position - _abs_offset_of(_selected_id), _drag_rect.size)
		rect_committed.emit(_selected_id, local)
	_reset_gesture()
	queue_redraw()


# Marquee rect in board space, normalized so size is non-negative regardless of drag
# direction.
func _marquee_board_rect() -> Rect2:
	var a := _canvas_to_board(_press_pos)
	var b := _canvas_to_board(_marquee_to)
	return Rect2(Vector2(minf(a.x, b.x), minf(a.y, b.y)), (a - b).abs())


# Per-member parent-relative rects for a rigid group-move. A member whose parent is
# also selected is "carried" by that ancestor (its local stays put; the ancestor's
# move shifts it), so applying the delta only to non-carried members moves the whole
# set rigidly by _group_delta on screen, never doubling a nested child.
func _build_group_edits() -> Array:
	var edits := []
	for id in _selection:
		if not _is_widget(id):
			continue
		var base := _document.get_window_rect(id)
		var carried := _selection.has(_document.get_parent_id(id))
		var new_pos := base.position if carried else base.position + _group_delta
		edits.append({"id": id, "rect": Rect2(new_pos, base.size)})
	return edits


# Every selectable widget in the visible screen whose absolute rect intersects a
# board-space marquee rect. Walks from the visible screen's root window children.
func _marquee_ids(board_rect: Rect2) -> PackedInt32Array:
	var out := PackedInt32Array()
	var screen_id := _visible_screen_id()
	if _document == null or screen_id < 0:
		return out
	var root := _document.get_screen_root_id(screen_id)
	if root < 0:
		return out
	for child in _document.get_child_ids(root):
		_collect_marquee(child, board_rect, out)
	return out


func _has_selection_widget() -> bool:
	return _document != null and _selected_id >= 0 \
		and _document.widget_exists(_selected_id) and not _document.is_screen(_selected_id)


func _reset_gesture() -> void:
	_gesture = Gesture.NONE
	_active_handle = -1
	_pressed = false
	_passed_threshold = false
	_drag_rect = Rect2()
	_group_start_rects = {}
	_group_delta = Vector2.ZERO
	_pending_collapse_id = -1
	_marquee_click_target = -1


func _update_cursor(pos: Vector2) -> void:
	if _space_held or _panning:
		mouse_default_cursor_shape = Control.CURSOR_DRAG
		return
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


# Track the widget under the cursor between gestures so _draw can outline it (the
# user sees what a click will select). A screen / empty hit clears the cue.
func _update_hover(pos: Vector2) -> void:
	var h := pick_widget_at(pos)
	if _document != null and (h < 0 or _document.is_screen(h)):
		h = -1
	if h != _hover_id:
		_hover_id = h
		queue_redraw()


func _on_mouse_exited() -> void:
	if _hover_id != -1:
		_hover_id = -1
		queue_redraw()


# Arrow keys nudge the selected widget by 1 board unit (Shift = grid step). Lives
# here (not the editor's undo shortcut) so it reuses the rect_committed commit path
# and defers to a focused spinner/tree that wants the arrows first: an unhandled
# arrow only reaches here when nothing else consumed it. A focused text field still
# guards explicitly (LineEdit ignores Up/Down, which would otherwise leak through).
func _unhandled_key_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	var text_focused := false
	var focus := get_viewport().gui_get_focus_owner()
	if focus is LineEdit or focus is TextEdit or focus is SpinBox:
		text_focused = true
	# Space arms pan-mode (Space+left-drag), tracking both press and release. Only
	# when no text field wants the key; consume it so it does not also scroll/toggle.
	if key.keycode == KEY_SPACE and not key.echo and not text_focused:
		if _space_held != key.pressed:
			_space_held = key.pressed
			_update_cursor(get_local_mouse_position())
		get_viewport().set_input_as_handled()
		return
	if not key.pressed or key.echo:
		return
	if not _has_selection_widget():
		return
	if text_focused:
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


# True when a widget rect (board space) is not fully inside the design board. A
# non-blocking authoring cue only: off-board placement is allowed (it may be
# intentional) and still saves. Rect2.encloses is strict containment, so this also
# flags negative origins and partial overhang; an exact board-fit stays on-board.
func _rect_off_board(r: Rect2) -> bool:
	if r.size.x <= 0.0 or r.size.y <= 0.0:
		return false
	return not Rect2(Vector2.ZERO, _menu_size).encloses(r)


# Whether the current selection sits off the board (for the workspace status line).
func is_selection_off_board() -> bool:
	return _rect_off_board(_abs_rect_of(_selected_id))


# --- Draw -----------------------------------------------------------------------

func _draw() -> void:
	_recompute_fit()
	draw_rect(Rect2(Vector2.ZERO, size), COL_BG)
	var board := Rect2(_eff_offset(), _menu_size * _eff_scale())
	draw_rect(board, COL_LETTERBOX)
	draw_rect(board, COL_BORDER, false, 1.0)

	# The interactive preview hides all authoring overlays so it reads like the real menu.
	if _interactive:
		return

	# Faint outline for every widget so tiny / empty / overlapping ones stay locatable.
	if _show_all_bounds:
		for wid in _visible_widget_ids():
			var ar := _abs_rect_of(wid)
			if ar.size.x > 0.0 and ar.size.y > 0.0:
				draw_rect(Rect2(_board_to_canvas(ar.position), ar.size * _eff_scale()), COL_ALLBOUNDS, false, 1.0)

	# Marquee rubber-band while box-selecting (canvas space, normalized).
	if _gesture == Gesture.MARQUEE and _passed_threshold:
		var m := Rect2(_press_pos, _marquee_to - _press_pos).abs()
		draw_rect(m, COL_MARQUEE_FILL)
		draw_rect(m, COL_HOVER, false, 1.0)
		return

	# During a move/resize drag, draw the live ghost(s) instead of the static selection
	# (tinted red the moment a rect leaves the board, so the warning tracks the gesture
	# live). A multi-move draws one ghost per member, each shifted by _group_delta.
	if _gesture != Gesture.NONE and _passed_threshold:
		if _gesture == Gesture.MOVE and _selection.size() > 1:
			for id in _group_start_rects:
				var start: Rect2 = _group_start_rects[id]
				var moved := Rect2(start.position + _group_delta, start.size)
				var gr := Rect2(_board_to_canvas(moved.position), moved.size * _eff_scale())
				draw_rect(gr, COL_OFFBOARD if _rect_off_board(moved) else COL_GHOST, false, 2.0)
		else:
			var ghost := Rect2(_board_to_canvas(_drag_rect.position), _drag_rect.size * _eff_scale())
			draw_rect(ghost, COL_OFFBOARD if _rect_off_board(_drag_rect) else COL_GHOST, false, 2.0)
		return

	# Hover cue (between gestures only): outline the widget a click would select,
	# unless it is already a selected member.
	if _hover_id >= 0 and not _selection.has(_hover_id):
		var hr := _abs_rect_of(_hover_id)
		if hr.size.x > 0.0 and hr.size.y > 0.0:
			draw_rect(Rect2(_board_to_canvas(hr.position), hr.size * _eff_scale()), COL_HOVER, false, 1.0)

	# Selection outline(s): every member is outlined; the active widget additionally
	# gets resize handles (single-select only) and the caption. Selection rides the
	# editor accent (single-sourced in the theme's EditorPalette).
	var col_select := Color(get_theme_color(&"accent", &"EditorPalette"), 0.95)
	for id in _selection:
		var r := _abs_rect_of(id)
		if r.size.x <= 0.0 or r.size.y <= 0.0:
			continue
		draw_rect(Rect2(_board_to_canvas(r.position), r.size * _eff_scale()),
			COL_OFFBOARD if _rect_off_board(r) else col_select, false, 2.0)
	var sel := _abs_rect_of(_selected_id)
	if sel.size.x > 0.0 and sel.size.y > 0.0:
		if _selection.size() <= 1:
			var half := HANDLE_SIZE * 0.5
			for c in _handle_centers():
				draw_rect(Rect2(c - Vector2(half, half), Vector2(HANDLE_SIZE, HANDLE_SIZE)), COL_HANDLE)
		_draw_selection_caption(sel)


# A small "Name (Type)" caption pinned to the selection's top-left (flipping below the
# rect when it would clip off the top), so the selected widget is unmistakable.
func _draw_selection_caption(sel_abs: Rect2) -> void:
	if not _has_selection_widget():
		return
	var font := get_theme_default_font()
	if font == null:
		return
	var fs := get_theme_default_font_size()
	if fs <= 0:
		fs = 14
	var caption: String
	if _selection.size() > 1:
		caption = "%d selected" % _selection.size()
	else:
		var type_name := _document.get_widget_type_name(_document.get_widget_type(_selected_id))
		var wname := _document.get_widget_name(_selected_id)
		caption = type_name if wname.is_empty() else "%s (%s)" % [wname, type_name]
	var text_size := font.get_string_size(caption, HORIZONTAL_ALIGNMENT_LEFT, -1, fs)
	var pad := Vector2(4, 2)
	var box_size := text_size + pad * 2.0
	var top_left := _board_to_canvas(sel_abs.position)
	var origin := Vector2(top_left.x, top_left.y - box_size.y - 2.0)
	if origin.y < 0.0:
		origin.y = top_left.y + 2.0
	draw_rect(Rect2(origin, box_size), COL_LABEL_BG)
	draw_string(font, origin + Vector2(pad.x, pad.y + font.get_ascent(fs)), caption,
		HORIZONTAL_ALIGNMENT_LEFT, -1, fs, COL_HANDLE)
