class_name FntEditor
extends Control

const FntEditorDocument = preload("res://modtools/fonts/fnt_editor_document.gd")
const FIRST_CHAR := 32
const GLYPH_COUNT := 224

var _document: FntEditorDocument
var _active_tool := "pencil"
var _selected_char := FIRST_CHAR
var _current_page := 0
var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []
var _clipboard := PackedByteArray()
var _clipboard_size := Vector2i.ZERO

var _atlas_canvas: TextureRect
var _glyph_grid: ItemList
var _glyph_inspector: Label
var _sample_preview: Label
var _page_spin: SpinBox


func _ready() -> void:
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


func set_active_tool(tool_name: String) -> void:
	_active_tool = tool_name


func select_char(char_code: int) -> void:
	if char_code < FIRST_CHAR or char_code >= FIRST_CHAR + GLYPH_COUNT:
		return
	_selected_char = char_code
	if _document != null and _document.resource != null:
		var page := _document.resource.get_glyph_page(char_code)
		if page >= 0:
			_current_page = page
	_refresh_selection()


func paint_pixel(page: int, x: int, y: int, alpha: int = 255) -> Error:
	if _document == null or _document.resource == null:
		return ERR_UNAVAILABLE
	var before := int(_document.resource.get_pixel_alpha(page, x, y))
	var next_alpha := 0 if _active_tool == "eraser" else clampi(alpha, 0, 255)
	if before == next_alpha:
		return OK
	var err := _document.resource.set_pixel_alpha(page, x, y, next_alpha)
	if err != OK:
		return err
	_push_pixel_operation([{"page": page, "x": x, "y": y, "before": before, "after": next_alpha}])
	_refresh_atlas()
	return OK


func clear_glyph(char_code: int = _selected_char) -> Error:
	if _document == null or _document.resource == null:
		return ERR_UNAVAILABLE
	var rect := _document.resource.get_glyph_rect(char_code)
	var page := _document.resource.get_glyph_page(char_code)
	if page < 0:
		return ERR_INVALID_PARAMETER
	var changes: Array[Dictionary] = []
	for y in range(rect.position.y, rect.position.y + rect.size.y):
		for x in range(rect.position.x, rect.position.x + rect.size.x):
			var before := int(_document.resource.get_pixel_alpha(page, x, y))
			if before == 0:
				continue
			changes.append({"page": page, "x": x, "y": y, "before": before, "after": 0})
			_document.resource.set_pixel_alpha(page, x, y, 0)
	if not changes.is_empty():
		_push_pixel_operation(changes)
		_refresh_atlas()
	return OK


func copy_glyph_alpha(char_code: int = _selected_char) -> void:
	_clipboard = PackedByteArray()
	_clipboard_size = Vector2i.ZERO
	if _document == null or _document.resource == null:
		return
	var rect := _document.resource.get_glyph_rect(char_code)
	var page := _document.resource.get_glyph_page(char_code)
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
	var rect := _document.resource.get_glyph_rect(char_code)
	var page := _document.resource.get_glyph_page(char_code)
	if page < 0:
		return ERR_INVALID_PARAMETER
	var w = mini(rect.size.x, _clipboard_size.x)
	var h = mini(rect.size.y, _clipboard_size.y)
	var changes: Array[Dictionary] = []
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
		_refresh_atlas()
	return OK


func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


func undo() -> void:
	if _document == null or _document.resource == null or _undo_stack.is_empty():
		return
	var op := _undo_stack.pop_back() as Dictionary
	_apply_changes(op["changes"], "before")
	_redo_stack.append(op)
	_refresh_atlas()


func redo() -> void:
	if _document == null or _document.resource == null or _redo_stack.is_empty():
		return
	var op := _redo_stack.pop_back() as Dictionary
	_apply_changes(op["changes"], "after")
	_undo_stack.append(op)
	_refresh_atlas()


func _build_ui() -> void:
	if _atlas_canvas != null:
		return
	var root := HSplitContainer.new()
	root.name = "RootSplit"
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(root)

	var left := VBoxContainer.new()
	left.name = "LeftTools"
	left.custom_minimum_size = Vector2(280, 0)
	left.size_flags_vertical = Control.SIZE_EXPAND_FILL
	left.add_theme_constant_override("separation", 8)
	root.add_child(left)

	var tool_row := HBoxContainer.new()
	tool_row.name = "ToolRow"
	left.add_child(tool_row)
	for tool_name in ["pencil", "eraser", "select", "move"]:
		var btn := Button.new()
		btn.text = tool_name.capitalize()
		btn.tooltip_text = "Use %s tool" % tool_name
		btn.pressed.connect(set_active_tool.bind(tool_name))
		tool_row.add_child(btn)

	_glyph_grid = ItemList.new()
	_glyph_grid.name = "GlyphGrid"
	_glyph_grid.unique_name_in_owner = true
	_glyph_grid.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_glyph_grid.select_mode = ItemList.SELECT_SINGLE
	_glyph_grid.item_selected.connect(func(index: int) -> void:
		select_char(FIRST_CHAR + index)
	)
	left.add_child(_glyph_grid)

	var right := VBoxContainer.new()
	right.name = "RightAuthoring"
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	right.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_theme_constant_override("separation", 8)
	root.add_child(right)

	var page_row := HBoxContainer.new()
	page_row.name = "PageRow"
	right.add_child(page_row)
	var page_label := Label.new()
	page_label.text = "Page"
	page_row.add_child(page_label)
	_page_spin = SpinBox.new()
	_page_spin.name = "PageSpin"
	_page_spin.min_value = 0
	_page_spin.max_value = 0
	_page_spin.step = 1
	_page_spin.value_changed.connect(func(value: float) -> void:
		_current_page = int(value)
		_refresh_atlas()
	)
	page_row.add_child(_page_spin)

	_atlas_canvas = TextureRect.new()
	_atlas_canvas.name = "AtlasCanvas"
	_atlas_canvas.unique_name_in_owner = true
	_atlas_canvas.custom_minimum_size = Vector2(512, 512)
	_atlas_canvas.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_atlas_canvas.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_atlas_canvas.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_atlas_canvas.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_atlas_canvas)

	_glyph_inspector = Label.new()
	_glyph_inspector.name = "GlyphInspector"
	_glyph_inspector.unique_name_in_owner = true
	_glyph_inspector.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	right.add_child(_glyph_inspector)

	_sample_preview = Label.new()
	_sample_preview.name = "SamplePreview"
	_sample_preview.unique_name_in_owner = true
	_sample_preview.text = "OpenNova 0123456789"
	_sample_preview.custom_minimum_size = Vector2(0, 48)
	right.add_child(_sample_preview)

	_assign_owner(root)


func _assign_owner(node: Node) -> void:
	node.owner = self
	for child in node.get_children():
		_assign_owner(child)


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
	_glyph_grid.clear()
	for code in range(FIRST_CHAR, FIRST_CHAR + GLYPH_COUNT):
		var display := char(code) if code > 32 else "space"
		_glyph_grid.add_item("%03d  %s" % [code, display])
	_glyph_grid.select(_selected_char - FIRST_CHAR)


func _refresh_atlas() -> void:
	if _atlas_canvas == null:
		return
	if _document == null or _document.resource == null:
		_atlas_canvas.texture = null
		return
	_page_spin.max_value = max(0, _document.resource.get_page_count() - 1)
	_page_spin.set_value_no_signal(_current_page)
	var image := _document.resource.get_page_image(_current_page)
	_atlas_canvas.texture = ImageTexture.create_from_image(image) if image != null else null


func _refresh_selection() -> void:
	if _glyph_grid != null:
		_glyph_grid.select(_selected_char - FIRST_CHAR)
	if _glyph_inspector == null:
		return
	if _document == null or _document.resource == null:
		_glyph_inspector.text = ""
		return
	var rect := _document.resource.get_glyph_rect(_selected_char)
	var page := _document.resource.get_glyph_page(_selected_char)
	_glyph_inspector.text = "char %d  page %d  rect %d,%d %dx%d" % [
		_selected_char,
		page,
		rect.position.x,
		rect.position.y,
		rect.size.x,
		rect.size.y,
	]


func _refresh_sample() -> void:
	if _sample_preview == null or _document == null or _document.resource == null:
		return
	var font := _document.resource.to_font_file()
	if font != null:
		_sample_preview.add_theme_font_override("font", font)


func _on_resource_loaded(_resource: NovaFntResource) -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_current_page = 0
	_refresh_all()


func _on_resource_changed() -> void:
	_refresh_selection()
	_refresh_sample()


func _push_pixel_operation(changes: Array[Dictionary]) -> void:
	_undo_stack.append({"changes": changes})
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
