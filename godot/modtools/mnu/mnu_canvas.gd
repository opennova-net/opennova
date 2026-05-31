class_name MnuCanvas
extends Control

# WYSIWYG preview surface for the Menus workspace. Hosts a live NovaMnuMenu in
# edit_mode (inert: no navigation, audio, or cursor side effects) scaled with
# uniform letterbox scaling to fit the document's authoring size (currently a
# fixed 640x480 design resolution, the standard for Joint Operations (JO) and
# newer menus; the engine does not yet parse a per-menu resolution), and draws a
# selection outline over the selected widget. M6 is read-only; the overlay
# gestures (drag/resize/snap) land in M8 on this same fit transform.

const COL_BG := Color(0.07, 0.08, 0.09)
const COL_LETTERBOX := Color(0.12, 0.13, 0.15)
const COL_BORDER := Color(1, 1, 1, 0.08)
const COL_SELECT := Color(0.84, 0.55, 0.29, 0.95)

var _preview: NovaMnuMenu
var _document: NovaMnuDocument
var _resource_root: NovaResourceRoot
var _text_resource: RtxtStringFile

var _menu_size := Vector2(640, 480)
var _fit_scale := 1.0
var _fit_offset := Vector2.ZERO
var _visible_screen_name := ""
var _highlight_rect := Rect2()


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_STOP
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


func set_highlight(rect: Rect2) -> void:
	_highlight_rect = rect
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
	# The canvas owns input (selection + future gestures); the preview never does.
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


func _draw() -> void:
	_recompute_fit()
	draw_rect(Rect2(Vector2.ZERO, size), COL_BG)
	var board := Rect2(_fit_offset, _menu_size * _fit_scale)
	draw_rect(board, COL_LETTERBOX)
	draw_rect(board, COL_BORDER, false, 1.0)
	if _highlight_rect.size.x > 0.0 and _highlight_rect.size.y > 0.0:
		var screen_rect := Rect2(
			_fit_offset + _highlight_rect.position * _fit_scale,
			_highlight_rect.size * _fit_scale)
		draw_rect(screen_rect, COL_SELECT, false, 2.0)
