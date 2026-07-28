class_name FntEditor
extends Control

# Interactive editor for Nova .fnt bitmap fonts. The painting surface is the inner
# FntCanvas (registered as the %AtlasCanvas unique node); all mutation funnels back
# through this script so undo stays centralized. paint_pixel/select_char/
# set_active_tool/can_undo/undo/redo keep their original signatures (test contract).

const FntEditorDocument = preload("res://modtools/fonts/fnt_editor_document.gd")
const EngineTextPreviewScript = preload("res://modtools/framework/engine_text_preview.gd")
const EDITOR_THEME_PATH := "res://modtools/editor/ui/theme/editor_theme.tres"
const FIRST_CHAR := 32
const GLYPH_COUNT := 224
const TEX_SIZE := 256

const TOOL_PENCIL := "pencil"
const TOOL_ERASER := "eraser"
const TOOL_FILL := "fill"
const TOOL_MOVE := "move"

var _document: FntEditorDocument
var _active_tool := TOOL_PENCIL
var _selected_char := FIRST_CHAR
var _current_page := 0
var _brush_size := 1
var _frame_glyph_mode := false

var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []
var _clipboard := PackedByteArray()
var _clipboard_size := Vector2i.ZERO

var _stroke_active := false
var _stroke_changes: Array[Dictionary] = []
var _batching := false
var _suppress_inspector_signals := false

var _move_active := false
var _move_start_rect := Rect2i()
var _move_origin := Vector2i.ZERO
var _move_delta := Vector2i.ZERO

var _atlas_canvas: Control
var _glyph_grid: ItemList
var _glyph_inspector: VBoxContainer
var _sample_preview: Control  # EngineTextPreview (framework F2 widget)
var _sample_edit: LineEdit
var _page_spin: SpinBox
var _zoom_label: Label
var _brush_spin: SpinBox
var _shadow_spin: SpinBox
var _glyph_page_spin: SpinBox
var _glyph_x_spin: SpinBox
var _glyph_y_spin: SpinBox
var _glyph_w_spin: SpinBox
var _glyph_h_spin: SpinBox
var _glyph_title: Label
var _glyph_hint: Label
var _tool_buttons := {}


func _ready() -> void:
	var th := load(EDITOR_THEME_PATH)
	if th != null:
		theme = th
	_build_ui()
	_refresh_all()


func set_document(value: FntEditorDocument) -> void:
	if _document == value:
		return
	if _document != null:
		if _document.resource_loaded.is_connected(_on_resource_loaded):
			_document.resource_loaded.disconnect(_on_resource_loaded)
		if _document.resource_changed.is_connected(_on_resource_changed):
			_document.resource_changed.disconnect(_on_resource_changed)
	_document = value
	if _document != null:
		_document.resource_loaded.connect(_on_resource_loaded)
		_document.resource_changed.connect(_on_resource_changed)
	_refresh_all()


# The workspace's dirty state derives from get_editor_document() (B6): this
# editor is that document surface, so it forwards its document's flag.
func is_dirty() -> bool:
	return _document != null and _document.is_dirty

func get_active_tool() -> String:
	return _active_tool


func set_active_tool(tool_name: String) -> void:
	_active_tool = tool_name
	_sync_tool_buttons()


func select_char(char_code: int) -> void:
	if char_code < FIRST_CHAR or char_code >= FIRST_CHAR + GLYPH_COUNT:
		return
	_selected_char = char_code
	if _document != null and _document.resource != null:
		var page: int = _document.resource.get_glyph_page(char_code)
		if page >= 0:
			_current_page = page
	_refresh_atlas()
	_refresh_selection()


func paint_pixel(page: int, x: int, y: int, alpha: int = 255) -> Error:
	if _document == null or _document.resource == null:
		return ERR_UNAVAILABLE
	var before := int(_document.resource.get_pixel_alpha(page, x, y))
	var next_alpha := 0 if _active_tool == TOOL_ERASER else clampi(alpha, 0, 255)
	if before == next_alpha:
		return OK
	var err: int = _document.resource.set_pixel_alpha(page, x, y, next_alpha)
	if err != OK:
		return err
	var change := {"page": page, "x": x, "y": y, "before": before, "after": next_alpha}
	if _stroke_active:
		_stroke_changes.append(change)
	else:
		_push_pixel_operation([change])
		_refresh_atlas()
	return OK


func copy_glyph_alpha(char_code: int = _selected_char) -> void:
	_clipboard = PackedByteArray()
	_clipboard_size = Vector2i.ZERO
	if _document == null or _document.resource == null:
		return
	var rect: Rect2i = _document.resource.get_glyph_rect(char_code)
	var page: int = _document.resource.get_glyph_page(char_code)
	if page < 0 or rect.size.x <= 0 or rect.size.y <= 0:
		return
	_clipboard_size = rect.size
	_clipboard.resize(rect.size.x * rect.size.y)
	var idx := 0
	for y in range(rect.position.y, rect.position.y + rect.size.y):
		for x in range(rect.position.x, rect.position.x + rect.size.x):
			_clipboard[idx] = _document.resource.get_pixel_alpha(page, x, y)
			idx += 1


func paste_glyph_alpha(char_code: int = _selected_char) -> Error:
	if _document == null or _document.resource == null or _clipboard.is_empty():
		return ERR_UNAVAILABLE
	var rect: Rect2i = _document.resource.get_glyph_rect(char_code)
	var page: int = _document.resource.get_glyph_page(char_code)
	if page < 0:
		return ERR_INVALID_PARAMETER
	var w = mini(rect.size.x, _clipboard_size.x)
	var h = mini(rect.size.y, _clipboard_size.y)
	var changes: Array[Dictionary] = []
	_begin_batch()
	for y in range(h):
		for x in range(w):
			var target_x := rect.position.x + x
			var target_y := rect.position.y + y
			var after := int(_clipboard[y * _clipboard_size.x + x])
			var before := int(_document.resource.get_pixel_alpha(page, target_x, target_y))
			if before == after:
				continue
			changes.append({"page": page, "x": target_x, "y": target_y, "before": before, "after": after})
			_document.resource.set_pixel_alpha(page, target_x, target_y, after)
	if not changes.is_empty():
		_push_pixel_operation(changes)
	_end_batch()
	return OK


func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


func undo() -> void:
	if _document == null or _document.resource == null or _undo_stack.is_empty():
		return
	var op := _undo_stack.pop_back() as Dictionary
	_begin_batch()
	_apply_changes(op.get("changes", []), "before")
	if op.has("rect"):
		var r: Dictionary = op["rect"]
		_document.resource.set_glyph_rect(int(r["char"]), int(r.get("before_page", r.get("page", 0))), r["before"])
	_redo_stack.append(op)
	_end_batch()


func redo() -> void:
	if _document == null or _document.resource == null or _redo_stack.is_empty():
		return
	var op := _redo_stack.pop_back() as Dictionary
	_begin_batch()
	_apply_changes(op.get("changes", []), "after")
	if op.has("rect"):
		var r: Dictionary = op["rect"]
		_document.resource.set_glyph_rect(int(r["char"]), int(r.get("after_page", r.get("page", 0))), r["after"])
	_undo_stack.append(op)
	_end_batch()


# --- stroke / batch bookkeeping -------------------------------------------------

func begin_stroke() -> void:
	_stroke_active = true
	_stroke_changes = []


func end_stroke() -> void:
	if not _stroke_active:
		return
	_stroke_active = false
	if not _stroke_changes.is_empty():
		_push_pixel_operation(_stroke_changes.duplicate())
	_stroke_changes = []
	_refresh_atlas()
	_refresh_selection()
	_refresh_sample()


func _begin_batch() -> void:
	_batching = true


func _end_batch() -> void:
	_batching = false
	_refresh_atlas()
	_refresh_selection()
	_refresh_sample()


# --- tool application (called from the canvas) ----------------------------------

func _brush_offsets() -> Array:
	if _brush_size <= 1:
		return [Vector2i.ZERO]
	var offs := []
	var start := -((_brush_size - 1) / 2)
	for dy in range(_brush_size):
		for dx in range(_brush_size):
			offs.append(Vector2i(start + dx, start + dy))
	return offs


func _apply_brush(src: Vector2i) -> void:
	for off in _brush_offsets():
		var x: int = src.x + off.x
		var y: int = src.y + off.y
		if x < 0 or y < 0 or x >= TEX_SIZE or y >= TEX_SIZE:
			continue
		paint_pixel(_current_page, x, y, 255)


func _apply_brush_line(a: Vector2i, b: Vector2i) -> void:
	var x0 := a.x
	var y0 := a.y
	var x1 := b.x
	var y1 := b.y
	var dx := absi(x1 - x0)
	var dy := -absi(y1 - y0)
	var sx := 1 if x0 < x1 else -1
	var sy := 1 if y0 < y1 else -1
	var err := dx + dy
	while true:
		_apply_brush(Vector2i(x0, y0))
		if x0 == x1 and y0 == y1:
			break
		var e2 := 2 * err
		if e2 >= dy:
			err += dy
			x0 += sx
		if e2 <= dx:
			err += dx
			y0 += sy


func _apply_fill(src: Vector2i) -> void:
	if _document == null or _document.resource == null:
		return
	var rect: Rect2i = _document.resource.get_glyph_rect(_selected_char)
	if rect.size.x <= 0 or rect.size.y <= 0:
		return
	var page: int = _document.resource.get_glyph_page(_selected_char)
	if page < 0 or not rect.has_point(src):
		return
	var target := int(_document.resource.get_pixel_alpha(page, src.x, src.y))
	var replacement := 0 if _active_tool == TOOL_ERASER else 255
	if target == replacement:
		return
	var changes: Array[Dictionary] = []
	var seen := {}
	var stack: Array[Vector2i] = [src]
	_begin_batch()
	while not stack.is_empty():
		var p: Vector2i = stack.pop_back()
		if seen.has(p) or not rect.has_point(p):
			continue
		seen[p] = true
		if int(_document.resource.get_pixel_alpha(page, p.x, p.y)) != target:
			continue
		_document.resource.set_pixel_alpha(page, p.x, p.y, replacement)
		changes.append({"page": page, "x": p.x, "y": p.y, "before": target, "after": replacement})
		stack.append(Vector2i(p.x + 1, p.y))
		stack.append(Vector2i(p.x - 1, p.y))
		stack.append(Vector2i(p.x, p.y + 1))
		stack.append(Vector2i(p.x, p.y - 1))
	if not changes.is_empty():
		_push_pixel_operation(changes)
	_end_batch()


func _begin_move_drag(src: Vector2i) -> void:
	if _document == null or _document.resource == null:
		return
	_move_active = true
	_move_start_rect = _document.resource.get_glyph_rect(_selected_char)
	_move_origin = src
	_move_delta = Vector2i.ZERO


func _update_move_drag(src: Vector2i) -> void:
	if not _move_active:
		return
	_move_delta = src - _move_origin
	if _atlas_canvas != null:
		_atlas_canvas.set_highlight(Rect2i(_move_start_rect.position + _move_delta, _move_start_rect.size))


func _commit_move_drag() -> void:
	if not _move_active:
		return
	_move_active = false
	if _document == null or _document.resource == null:
		return
	var old_rect := _move_start_rect
	if _move_delta == Vector2i.ZERO or old_rect.size.x <= 0 or old_rect.size.y <= 0:
		_refresh_selection()
		return
	var page: int = _document.resource.get_glyph_page(_selected_char)
	if page < 0:
		_refresh_selection()
		return
	var new_pos := old_rect.position + _move_delta
	new_pos.x = clampi(new_pos.x, 0, TEX_SIZE - old_rect.size.x)
	new_pos.y = clampi(new_pos.y, 0, TEX_SIZE - old_rect.size.y)
	var new_rect := Rect2i(new_pos, old_rect.size)
	# Snapshot the source block before mutating.
	var block := PackedByteArray()
	block.resize(old_rect.size.x * old_rect.size.y)
	var bi := 0
	for yy in range(old_rect.size.y):
		for xx in range(old_rect.size.x):
			block[bi] = _document.resource.get_pixel_alpha(page, old_rect.position.x + xx, old_rect.position.y + yy)
			bi += 1
	var changes: Array[Dictionary] = []
	_begin_batch()
	var union := old_rect.merge(new_rect)
	for yy in range(union.position.y, union.position.y + union.size.y):
		for xx in range(union.position.x, union.position.x + union.size.x):
			var p := Vector2i(xx, yy)
			var after := 0
			if new_rect.has_point(p):
				var rel := p - new_pos
				after = int(block[rel.y * old_rect.size.x + rel.x])
			var before := int(_document.resource.get_pixel_alpha(page, xx, yy))
			if before != after:
				changes.append({"page": page, "x": xx, "y": yy, "before": before, "after": after})
				_document.resource.set_pixel_alpha(page, xx, yy, after)
	_document.resource.set_glyph_rect(_selected_char, page, new_rect)
	_push_operation({
		"changes": changes,
		"rect": {"char": _selected_char, "before": old_rect, "after": new_rect, "before_page": page, "after_page": page},
	})
	_move_delta = Vector2i.ZERO
	_end_batch()


# --- UI construction ------------------------------------------------------------

func _build_ui() -> void:
	if _atlas_canvas != null:
		return
	var root := HSplitContainer.new()
	root.name = "RootSplit"
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.split_offset = 320
	add_child(root)

	# ----- left column ------------------------------------------------------
	var left := VBoxContainer.new()
	left.name = "LeftColumn"
	left.custom_minimum_size = Vector2(300, 0)
	left.size_flags_vertical = Control.SIZE_EXPAND_FILL
	left.add_theme_constant_override("separation", 8)
	root.add_child(left)

	var tool_box := _make_panel_box(left, 8, 6)
	_add_heading(tool_box, "Tools")

	var tool_row := HBoxContainer.new()
	tool_row.add_theme_constant_override("separation", 4)
	tool_box.add_child(tool_row)
	var tool_group := ButtonGroup.new()
	for spec in [[TOOL_PENCIL, "Pencil", "B"], [TOOL_ERASER, "Eraser", "E"], [TOOL_FILL, "Fill", "G"], [TOOL_MOVE, "Move", "M"]]:
		var btn := Button.new()
		btn.text = spec[1]
		btn.tooltip_text = "%s tool (%s)" % [spec[1], spec[2]]
		btn.toggle_mode = true
		btn.button_group = tool_group
		btn.focus_mode = Control.FOCUS_NONE
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var tn: String = spec[0]
		btn.pressed.connect(func() -> void: set_active_tool(tn))
		tool_row.add_child(btn)
		_tool_buttons[tn] = btn
	_tool_buttons[TOOL_PENCIL].set_pressed_no_signal(true)

	_brush_spin = _add_spin_row(tool_box, "Brush", 1, 16, 1)
	_brush_spin.value = 1
	_brush_spin.value_changed.connect(func(v: float) -> void: _brush_size = int(v))

	var frame_check := CheckButton.new()
	frame_check.text = "Frame glyph"
	frame_check.tooltip_text = "Zoom the canvas to the selected glyph"
	frame_check.toggled.connect(_on_frame_toggled)
	tool_box.add_child(frame_check)

	_add_heading(left, "Glyphs")
	_glyph_grid = ItemList.new()
	_glyph_grid.name = "GlyphGrid"
	_glyph_grid.unique_name_in_owner = true
	_glyph_grid.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_glyph_grid.select_mode = ItemList.SELECT_SINGLE
	_glyph_grid.icon_mode = ItemList.ICON_MODE_TOP
	_glyph_grid.fixed_icon_size = Vector2i(28, 28)
	_glyph_grid.max_columns = 0
	_glyph_grid.same_column_width = true
	_glyph_grid.item_selected.connect(func(index: int) -> void: select_char(FIRST_CHAR + index))
	left.add_child(_glyph_grid)

	# ----- right column -----------------------------------------------------
	var right := VBoxContainer.new()
	right.name = "RightColumn"
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	right.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_theme_constant_override("separation", 8)
	root.add_child(right)

	var page_row := HBoxContainer.new()
	page_row.add_theme_constant_override("separation", 6)
	right.add_child(page_row)
	var page_lbl := Label.new()
	page_lbl.text = "Page"
	page_lbl.theme_type_variation = &"Muted"
	page_lbl.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	page_row.add_child(page_lbl)
	_page_spin = SpinBox.new()
	_page_spin.min_value = 0
	_page_spin.max_value = 0
	_page_spin.step = 1
	_page_spin.value_changed.connect(_on_page_spin_changed)
	page_row.add_child(_page_spin)
	var page_spacer := Control.new()
	page_spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	page_row.add_child(page_spacer)
	page_row.add_child(_make_tool_button("-", "Zoom out (-)", func() -> void: _zoom_canvas(1.0 / 1.2)))
	_zoom_label = Label.new()
	_zoom_label.text = "8x"
	_zoom_label.theme_type_variation = &"Muted"
	_zoom_label.custom_minimum_size = Vector2(44, 0)
	_zoom_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_zoom_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	page_row.add_child(_zoom_label)
	page_row.add_child(_make_tool_button("+", "Zoom in (+)", func() -> void: _zoom_canvas(1.2)))
	page_row.add_child(_make_tool_button("Fit", "Fit page to view", _fit_canvas))
	page_row.add_child(_make_tool_button("Generate from font…", "Rasterize a TTF or system font into this font", _on_generate_pressed))

	var canvas_panel := PanelContainer.new()
	canvas_panel.theme_type_variation = &"FlatPanel"
	canvas_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	canvas_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(canvas_panel)
	_atlas_canvas = FntCanvas.new()
	_atlas_canvas.name = "AtlasCanvas"
	_atlas_canvas.unique_name_in_owner = true
	_atlas_canvas.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_atlas_canvas.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_atlas_canvas.setup(self)
	canvas_panel.add_child(_atlas_canvas)

	var insp_box := _make_panel_box(right, 10, 6)
	_glyph_inspector = VBoxContainer.new()
	_glyph_inspector.name = "GlyphInspector"
	_glyph_inspector.unique_name_in_owner = true
	_glyph_inspector.add_theme_constant_override("separation", 6)
	insp_box.add_child(_glyph_inspector)
	_build_inspector_rows(_glyph_inspector)

	# FNT-1 (maturity program): the type-a-line sample renders through the GAME's
	# draw path — the F2 EngineTextPreview widget (NovaFntResource.to_font_file +
	# HudText.draw_text) — beside the glyph canvas, which stays as the paint
	# surface. The previous Godot Label was an editor-drawn truth-claim. A .fnt
	# carries a single face (the hi/lo pairing is hudpos.def naming two separate
	# files), so the panel previews the one edited face.
	var sample_box := _make_panel_box(right, 10, 6)
	_add_heading(sample_box, "Game preview")
	_sample_edit = LineEdit.new()
	_sample_edit.name = "SampleEdit"
	_sample_edit.unique_name_in_owner = true
	_sample_edit.text = "OpenNova 0123456789"
	_sample_edit.tooltip_text = "Type a line to see how the game draws it with this font."
	_sample_edit.text_changed.connect(_on_sample_text_changed)
	sample_box.add_child(_sample_edit)
	_sample_preview = EngineTextPreviewScript.new()
	_sample_preview.name = "SamplePreview"
	_sample_preview.unique_name_in_owner = true
	_sample_preview.custom_minimum_size = Vector2(0, 56)
	_sample_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_sample_preview.set_sample_text(_sample_edit.text)
	sample_box.add_child(_sample_preview)

	_assign_owner(root)


func _build_inspector_rows(box: VBoxContainer) -> void:
	_glyph_title = Label.new()
	_glyph_title.theme_type_variation = &"Heading"
	_glyph_title.text = "Glyph"
	box.add_child(_glyph_title)

	_shadow_spin = _add_spin_row(box, "Shadow", -64, 64, 1)
	_shadow_spin.tooltip_text = "Shadow offset baked into glyph advance (-3 = shadowed)"
	_shadow_spin.value_changed.connect(func(v: float) -> void:
		if not _suppress_inspector_signals:
			_on_shadow_changed(int(v)))

	_glyph_page_spin = _add_spin_row(box, "Page", 0, 15, 1)
	_glyph_page_spin.value_changed.connect(func(_v: float) -> void: _apply_glyph_meta())

	var xy := _add_spin_pair(box, "Pos X/Y", 0, 255, 1)
	_glyph_x_spin = xy[0]
	_glyph_y_spin = xy[1]
	var wh := _add_spin_pair(box, "Size W/H", 0, 256, 1)
	_glyph_w_spin = wh[0]
	_glyph_h_spin = wh[1]
	for sp in [_glyph_x_spin, _glyph_y_spin, _glyph_w_spin, _glyph_h_spin]:
		sp.value_changed.connect(func(_v: float) -> void: _apply_glyph_meta())

	_glyph_hint = Label.new()
	_glyph_hint.theme_type_variation = &"Muted"
	_glyph_hint.text = ""
	box.add_child(_glyph_hint)


func _make_panel_box(parent: Control, margin: int, separation: int) -> VBoxContainer:
	var panel := PanelContainer.new()
	panel.theme_type_variation = &"FlatPanel"
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(panel)
	var mc := MarginContainer.new()
	for side in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		mc.add_theme_constant_override(side, margin)
	panel.add_child(mc)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", separation)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mc.add_child(box)
	return box


func _add_heading(parent: Control, text: String) -> void:
	var lbl := Label.new()
	lbl.text = text
	lbl.theme_type_variation = &"Heading"
	parent.add_child(lbl)


func _make_tool_button(text: String, tooltip: String, on_press: Callable) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.tooltip_text = tooltip
	btn.focus_mode = Control.FOCUS_NONE
	btn.pressed.connect(on_press)
	return btn


func _add_spin_row(parent: Control, label_text: String, minv: float, maxv: float, step: float) -> SpinBox:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var lbl := Label.new()
	lbl.text = label_text
	lbl.theme_type_variation = &"Muted"
	lbl.custom_minimum_size = Vector2(70, 0)
	lbl.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(lbl)
	var spin := SpinBox.new()
	spin.min_value = minv
	spin.max_value = maxv
	spin.step = step
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spin)
	parent.add_child(row)
	return spin


func _add_spin_pair(parent: Control, label_text: String, minv: float, maxv: float, step: float) -> Array:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_theme_constant_override("separation", 6)
	var lbl := Label.new()
	lbl.text = label_text
	lbl.theme_type_variation = &"Muted"
	lbl.custom_minimum_size = Vector2(70, 0)
	lbl.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(lbl)
	var a := SpinBox.new()
	a.min_value = minv
	a.max_value = maxv
	a.step = step
	a.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(a)
	var b := SpinBox.new()
	b.min_value = minv
	b.max_value = maxv
	b.step = step
	b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(b)
	parent.add_child(row)
	return [a, b]


func _assign_owner(node: Node) -> void:
	node.owner = self
	for child in node.get_children():
		_assign_owner(child)


# --- inspector editing ----------------------------------------------------------

func _on_shadow_changed(v: int) -> void:
	if _document == null or _document.resource == null:
		return
	_document.resource.set_shadow_offset(v)
	_refresh_sample()


func _apply_glyph_meta() -> void:
	if _suppress_inspector_signals or _document == null or _document.resource == null:
		return
	var page := int(_glyph_page_spin.value)
	var rect := Rect2i(int(_glyph_x_spin.value), int(_glyph_y_spin.value), int(_glyph_w_spin.value), int(_glyph_h_spin.value))
	var old_rect: Rect2i = _document.resource.get_glyph_rect(_selected_char)
	var old_page: int = _document.resource.get_glyph_page(_selected_char)
	if rect == old_rect and page == old_page:
		return
	_document.resource.set_glyph_rect(_selected_char, page, rect)
	_push_operation({
		"changes": [],
		"rect": {"char": _selected_char, "before": old_rect, "after": rect, "before_page": old_page, "after_page": page},
	})
	_current_page = page
	_refresh_atlas()
	_refresh_selection()


# --- refresh --------------------------------------------------------------------

func _refresh_all() -> void:
	if not is_node_ready():
		return
	_refresh_glyph_grid()
	_refresh_atlas()
	_refresh_selection()
	_refresh_sample()


func _refresh_glyph_grid() -> void:
	if _glyph_grid == null:
		return
	var prev := _selected_char
	_glyph_grid.clear()
	for code in range(FIRST_CHAR, FIRST_CHAR + GLYPH_COUNT):
		var idx := _glyph_grid.add_item("%d %s" % [code, _glyph_label(code)])
		var thumb := _glyph_thumbnail(code)
		if thumb != null:
			_glyph_grid.set_item_icon(idx, thumb)
		else:
			_glyph_grid.set_item_custom_fg_color(idx, Color(0.45, 0.47, 0.52))
	_glyph_grid.select(prev - FIRST_CHAR)


func _refresh_atlas() -> void:
	if _atlas_canvas == null:
		return
	if _document == null or _document.resource == null:
		_atlas_canvas.set_page_texture(null)
		return
	var pages: int = _document.resource.get_page_count()
	_page_spin.max_value = max(0, pages - 1)
	_page_spin.set_value_no_signal(_current_page)
	if _glyph_page_spin != null:
		_glyph_page_spin.max_value = max(0, pages - 1)
	_atlas_canvas.set_page_texture(_page_texture(_current_page))


func refresh_atlas_texture() -> void:
	# Cheap mid-stroke refresh: rebuild only the page texture, no spins or font.
	if _atlas_canvas == null or _document == null or _document.resource == null:
		return
	_atlas_canvas.set_page_texture(_page_texture(_current_page))


func _page_texture(page: int) -> Texture2D:
	var image: Image = _document.resource.get_page_image(page)
	if image == null:
		return null
	return ImageTexture.create_from_image(image)


func _refresh_selection() -> void:
	if _glyph_grid != null:
		_glyph_grid.select(_selected_char - FIRST_CHAR)
	if _document == null or _document.resource == null or _glyph_inspector == null:
		return
	var rect: Rect2i = _document.resource.get_glyph_rect(_selected_char)
	var page: int = _document.resource.get_glyph_page(_selected_char)
	_suppress_inspector_signals = true
	if _glyph_title != null:
		_glyph_title.text = "Glyph %d  '%s'" % [_selected_char, _glyph_label(_selected_char)]
	if _shadow_spin != null:
		_shadow_spin.set_value_no_signal(_document.resource.get_shadow_offset())
	if _glyph_page_spin != null:
		_glyph_page_spin.set_value_no_signal(maxi(0, page))
	if _glyph_x_spin != null:
		_glyph_x_spin.set_value_no_signal(rect.position.x)
		_glyph_y_spin.set_value_no_signal(rect.position.y)
		_glyph_w_spin.set_value_no_signal(rect.size.x)
		_glyph_h_spin.set_value_no_signal(rect.size.y)
	_suppress_inspector_signals = false
	if _glyph_hint != null:
		if rect.size.x > 0 and rect.size.y > 0:
			_glyph_hint.text = "%d x %d px on page %d" % [rect.size.x, rect.size.y, page]
		else:
			_glyph_hint.text = "empty"
	if _atlas_canvas != null:
		_atlas_canvas.set_highlight(rect if page >= 0 else Rect2i())
		if _frame_glyph_mode and rect.size.x > 0 and rect.size.y > 0:
			_atlas_canvas.frame_rect(rect)


func _refresh_sample() -> void:
	# Re-adopt the edited document's font through the engine view (a missing
	# document/resource clears the panel to its load hint).
	if _sample_preview == null:
		return
	_sample_preview.set_font_from_fnt(_document.resource if _document != null else null)
	if _sample_edit != null:
		_sample_preview.set_sample_text(_sample_edit.text)


func _glyph_thumbnail(code: int) -> Texture2D:
	if _document == null or _document.resource == null:
		return null
	var rect: Rect2i = _document.resource.get_glyph_rect(code)
	if rect.size.x <= 0 or rect.size.y <= 0:
		return null
	var page: int = _document.resource.get_glyph_page(code)
	if page < 0:
		return null
	var img: Image = _document.resource.get_page_image(page)
	if img == null:
		return null
	var clamped := rect.intersection(Rect2i(0, 0, img.get_width(), img.get_height()))
	if clamped.size.x <= 0 or clamped.size.y <= 0:
		return null
	return ImageTexture.create_from_image(img.get_region(clamped))


func _glyph_label(code: int) -> String:
	if code == 32:
		return "space"
	return char(code)


func _sync_tool_buttons() -> void:
	for tn in _tool_buttons:
		var btn: Button = _tool_buttons[tn]
		btn.set_pressed_no_signal(tn == _active_tool)


func _zoom_canvas(factor: float) -> void:
	if _atlas_canvas != null:
		_atlas_canvas.zoom_at(_atlas_canvas.size * 0.5, factor)


func _fit_canvas() -> void:
	if _atlas_canvas != null:
		_atlas_canvas.fit()


func _on_page_spin_changed(v: float) -> void:
	_current_page = int(v)
	_refresh_atlas()
	_refresh_selection()


func _on_frame_toggled(pressed: bool) -> void:
	_frame_glyph_mode = pressed
	if pressed:
		select_char(_selected_char)


func _on_sample_text_changed(text: String) -> void:
	if _sample_preview != null:
		_sample_preview.set_sample_text(text)


func _on_zoom_changed(z: float) -> void:
	if _zoom_label != null:
		_zoom_label.text = "%dx" % int(round(z))


func _on_generate_pressed() -> void:
	if _document == null:
		return
	var dialog_script := load("res://modtools/fonts/fnt_generate_dialog.gd") as GDScript
	if dialog_script == null:
		return
	var dlg: AcceptDialog = dialog_script.new()
	add_child(dlg)
	dlg.generate_requested.connect(_on_generate_requested)
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()


func _on_generate_requested(font: Font, px_size: int, flags: int) -> void:
	if _document == null:
		return
	var err := _document.generate_from_font(font, px_size, flags)
	if err != OK and _glyph_hint != null:
		_glyph_hint.text = "Generation failed (font too large for 16 pages? try a smaller size)"


# --- document signals -----------------------------------------------------------

func _on_resource_loaded(_resource) -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_current_page = 0
	_selected_char = FIRST_CHAR
	_refresh_all()


func _on_resource_changed() -> void:
	if _stroke_active or _batching:
		return
	_refresh_atlas()
	_refresh_selection()
	_refresh_sample()


# --- undo bookkeeping -----------------------------------------------------------

func _push_pixel_operation(changes: Array[Dictionary]) -> void:
	_push_operation({"changes": changes})


func _push_operation(op: Dictionary) -> void:
	_undo_stack.append(op)
	_redo_stack.clear()


func _apply_changes(changes: Array, key: String) -> void:
	for change in changes:
		var c := change as Dictionary
		_document.resource.set_pixel_alpha(
			int(c["page"]),
			int(c["x"]),
			int(c["y"]),
			int(c[key])
		)


# --- keyboard -------------------------------------------------------------------

func _shortcut_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var k := event as InputEventKey
	if not k.pressed or k.echo:
		return
	var fo := get_viewport().gui_get_focus_owner()
	if fo is LineEdit or fo is TextEdit or fo is SpinBox:
		return
	var handled := true
	if k.ctrl_pressed:
		# Undo/redo shortcuts live in the shell's _shortcut_input (B6).
		match k.keycode:
			KEY_C:
				copy_glyph_alpha()
			KEY_V:
				paste_glyph_alpha()
			_:
				handled = false
	else:
		match k.keycode:
			KEY_B:
				set_active_tool(TOOL_PENCIL)
			KEY_E:
				set_active_tool(TOOL_ERASER)
			KEY_G:
				set_active_tool(TOOL_FILL)
			KEY_M:
				set_active_tool(TOOL_MOVE)
			KEY_BRACKETLEFT, KEY_LEFT:
				select_char(_selected_char - 1)
			KEY_BRACKETRIGHT, KEY_RIGHT:
				select_char(_selected_char + 1)
			KEY_EQUAL, KEY_KP_ADD:
				_zoom_canvas(1.2)
			KEY_MINUS, KEY_KP_SUBTRACT:
				_zoom_canvas(1.0 / 1.2)
			_:
				handled = false
	if handled:
		get_viewport().set_input_as_handled()


# --- painting surface -----------------------------------------------------------

class FntCanvas extends Control:
	const TEX := 256
	const GRID_THRESHOLD := 6.0
	const COL_BG := Color(0.07, 0.08, 0.09)
	const COL_BOARD := Color(0.18, 0.20, 0.23)
	const COL_GRID := Color(1, 1, 1, 0.07)
	const COL_HOVER := Color(1, 1, 1, 0.5)

	var editor
	var texture: Texture2D
	var zoom := 8.0
	var pan := Vector2(8, 8)
	var highlight := Rect2i()
	var hover := Vector2i(-1, -1)
	var painting := false
	var panning := false
	var moving := false
	var last_src := Vector2i.ZERO
	var _fitted := false

	func setup(p_editor) -> void:
		editor = p_editor
		texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		mouse_filter = Control.MOUSE_FILTER_STOP
		focus_mode = Control.FOCUS_CLICK
		clip_contents = true

	func set_page_texture(t: Texture2D) -> void:
		texture = t
		queue_redraw()

	func set_highlight(r: Rect2i) -> void:
		highlight = r
		queue_redraw()

	func fit() -> void:
		var z := floorf(minf(size.x, size.y) / float(TEX))
		zoom = maxf(1.0, z)
		pan = (size - Vector2(TEX, TEX) * zoom) * 0.5
		_emit_zoom()
		queue_redraw()

	func frame_rect(r: Rect2i) -> void:
		if r.size.x <= 0 or r.size.y <= 0:
			return
		var z := floorf(minf(size.x / float(r.size.x), size.y / float(r.size.y)))
		zoom = clampf(z, 1.0, 64.0)
		var center := Vector2(r.position) + Vector2(r.size) * 0.5
		pan = size * 0.5 - center * zoom
		_emit_zoom()
		queue_redraw()

	func zoom_at(local: Vector2, factor: float) -> void:
		var s := (local - pan) / zoom
		zoom = clampf(zoom * factor, 1.0, 64.0)
		pan = local - s * zoom
		_emit_zoom()
		queue_redraw()

	func _emit_zoom() -> void:
		if editor != null:
			editor._on_zoom_changed(zoom)

	func _screen_to_src(p: Vector2) -> Vector2i:
		var v := (p - pan) / zoom
		return Vector2i(int(floor(v.x)), int(floor(v.y)))

	func _src_to_screen(s: Vector2i) -> Rect2:
		return Rect2(pan + Vector2(s) * zoom, Vector2(zoom, zoom))

	func _in_bounds(s: Vector2i) -> bool:
		return s.x >= 0 and s.y >= 0 and s.x < TEX and s.y < TEX

	func _draw() -> void:
		if not _fitted and size.x > 4.0 and size.y > 4.0:
			_fitted = true
			fit()
		draw_rect(Rect2(Vector2.ZERO, size), COL_BG)
		var board := Rect2(pan, Vector2(TEX, TEX) * zoom)
		draw_rect(board, COL_BOARD)
		if texture != null:
			draw_texture_rect(texture, board, false)
		if zoom >= GRID_THRESHOLD:
			for i in range(0, TEX + 1):
				var x := pan.x + i * zoom
				draw_line(Vector2(x, pan.y), Vector2(x, pan.y + TEX * zoom), COL_GRID)
				var y := pan.y + i * zoom
				draw_line(Vector2(pan.x, y), Vector2(pan.x + TEX * zoom, y), COL_GRID)
		if highlight.size.x > 0 and highlight.size.y > 0:
			var hr := Rect2(pan + Vector2(highlight.position) * zoom, Vector2(highlight.size) * zoom)
			draw_rect(hr, Color(get_theme_color(&"accent", &"EditorPalette"), 0.9), false, 2.0)
		if _in_bounds(hover):
			var hsize := 1
			if editor != null:
				hsize = max(1, editor._brush_size)
			var off := -((hsize - 1) / 2)
			draw_rect(Rect2(pan + Vector2(hover.x + off, hover.y + off) * zoom, Vector2(hsize, hsize) * zoom), COL_HOVER, false, 1.0)

	func _gui_input(event: InputEvent) -> void:
		if event is InputEventMouseButton:
			var mb := event as InputEventMouseButton
			if mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
				zoom_at(mb.position, 1.2)
				accept_event()
			elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
				zoom_at(mb.position, 1.0 / 1.2)
				accept_event()
			elif mb.button_index == MOUSE_BUTTON_MIDDLE or mb.button_index == MOUSE_BUTTON_RIGHT:
				panning = mb.pressed
				accept_event()
			elif mb.button_index == MOUSE_BUTTON_LEFT and editor != null:
				if mb.pressed:
					grab_focus()
					var src := _screen_to_src(mb.position)
					var tool: String = editor.get_active_tool()
					if tool == "move":
						moving = true
						editor._begin_move_drag(src)
					elif tool == "fill":
						editor._apply_fill(src)
					else:
						painting = true
						last_src = src
						editor.begin_stroke()
						editor._apply_brush(src)
						editor.refresh_atlas_texture()
				else:
					if painting:
						painting = false
						editor.end_stroke()
					elif moving:
						moving = false
						editor._commit_move_drag()
				accept_event()
		elif event is InputEventMouseMotion:
			var mm := event as InputEventMouseMotion
			hover = _screen_to_src(mm.position)
			if panning:
				pan += mm.relative
			elif painting and editor != null:
				var src := _screen_to_src(mm.position)
				if src != last_src:
					editor._apply_brush_line(last_src, src)
					editor.refresh_atlas_texture()
					last_src = src
			elif moving and editor != null:
				editor._update_move_drag(_screen_to_src(mm.position))
			queue_redraw()
