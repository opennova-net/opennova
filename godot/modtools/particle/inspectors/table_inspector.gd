class_name ParticleTableInspector
extends Control

## Lists [tabledef] entries; selecting one renders the 32×8 (256-byte logical
## curve) as a line graph. Drag across the graph to reshape the curve; presets
## give a quick ramp / flat / invert. Edits feed the live preview.

@onready var _list: ItemList = %TableList
@onready var _empty_label: Label = %EmptyLabel
@onready var _id_edit: LineEdit = %IdEdit
@onready var _curve_view: Control = %CurveView
@onready var _curve_caption: Label = %CurveCaption

var _editor: ParticleEditor
var _workspace
var _tables: Array = []
var _dup_button: Button
var _del_button: Button
var _last_paint_index := -1
var _last_paint_value := 0
var _suppress_signals := false


func _ready() -> void:
	_build_toolbar()
	_list.item_selected.connect(_on_item_selected)
	_id_edit.text_changed.connect(_on_id_changed)
	_curve_view.draw.connect(_on_curve_draw)
	_curve_view.mouse_filter = Control.MOUSE_FILTER_STOP
	_curve_view.gui_input.connect(_on_curve_input)
	_curve_view.tooltip_text = "Drag to shape the curve (left = particle birth, right = death)."
	_build_curve_tools()
	_refresh()


func _build_curve_tools() -> void:
	var row := HBoxContainer.new()
	add_child(row)
	move_child(row, _curve_view.get_index())  # just above the curve graph
	var hint := Label.new()
	hint.theme_type_variation = &"Muted"
	hint.text = "Drag the graph to edit:"
	row.add_child(hint)
	_add_curve_tool(row, "Ramp", _on_curve_ramp)
	_add_curve_tool(row, "Flat", _on_curve_flat)
	_add_curve_tool(row, "Invert", _on_curve_invert)


func _add_curve_tool(parent: Control, text: String, cb: Callable) -> void:
	var btn := Button.new()
	btn.text = text
	btn.focus_mode = Control.FOCUS_NONE
	parent.add_child(btn)
	btn.pressed.connect(cb)


func _build_toolbar() -> void:
	var toolbar := HBoxContainer.new()
	add_child(toolbar)
	move_child(toolbar, _list.get_index())
	var add_btn := Button.new()
	add_btn.text = "+ Add"
	add_btn.tooltip_text = "Add a new curve table (linear ramp)"
	toolbar.add_child(add_btn)
	add_btn.pressed.connect(_on_add_pressed)
	_dup_button = Button.new()
	_dup_button.text = "Duplicate"
	toolbar.add_child(_dup_button)
	_dup_button.pressed.connect(_on_duplicate_pressed)
	_del_button = Button.new()
	_del_button.text = "Delete"
	toolbar.add_child(_del_button)
	_del_button.pressed.connect(_on_delete_pressed)


func _on_add_pressed() -> void:
	if _workspace != null and _workspace.has_method("add_table"):
		_workspace.add_table()


func _on_duplicate_pressed() -> void:
	if _editor == null or _editor.current_table == null:
		return
	if _workspace != null and _workspace.has_method("duplicate_table"):
		_workspace.duplicate_table(_editor.current_table)


func _on_delete_pressed() -> void:
	if _editor == null or _editor.current_table == null:
		return
	if _workspace != null and _workspace.has_method("remove_table"):
		_workspace.remove_table(_editor.current_table)


func set_particle_editor(value: ParticleEditor) -> void:
	if _editor != null:
		if _editor.document_changed.is_connected(_refresh):
			_editor.document_changed.disconnect(_refresh)
		if _editor.selection_changed.is_connected(_refresh_selection):
			_editor.selection_changed.disconnect(_refresh_selection)
	_editor = value
	if _editor != null:
		_editor.document_changed.connect(_refresh)
		_editor.selection_changed.connect(_refresh_selection)
	_refresh()


func set_workspace(value) -> void:
	_workspace = value


func _refresh() -> void:
	_tables.clear()
	_list.clear()
	if _editor != null and _editor.particle_file != null:
		var src: Array = _editor.particle_file.tables
		for entry in src:
			var t: NovaParticleTable = entry
			if t != null:
				_tables.append(t)
				_list.add_item(t.id)
	_refresh_selection()
	_empty_label.visible = _tables.is_empty()
	var has_selection := _editor != null and _editor.current_table != null
	_id_edit.editable = has_selection
	if _dup_button != null:
		_dup_button.disabled = not has_selection
	if _del_button != null:
		_del_button.disabled = not has_selection


func _refresh_selection() -> void:
	if _editor == null or _editor.current_table == null:
		_suppress_signals = true
		_id_edit.text = ""
		_suppress_signals = false
		_curve_caption.text = "(no table selected)"
		_curve_view.queue_redraw()
		return
	var idx := _tables.find(_editor.current_table)
	if idx >= 0:
		_list.select(idx)
	_suppress_signals = true
	_id_edit.text = _editor.current_table.id
	_suppress_signals = false
	_update_caption()
	_curve_view.queue_redraw()


func _update_caption() -> void:
	if _editor == null or _editor.current_table == null:
		return
	var data: PackedByteArray = _editor.current_table.get_data()
	var lo := 255
	var hi := 0
	var total := 0
	for v in data:
		lo = mini(lo, v)
		hi = maxi(hi, v)
		total += v
	var avg := (total / data.size()) if data.size() > 0 else 0
	_curve_caption.text = "%s — min %d · max %d · avg %d" % [_editor.current_table.id, lo, hi, avg]


func _on_item_selected(idx: int) -> void:
	if idx < 0 or idx >= _tables.size():
		return
	if _workspace != null and _workspace.has_method("select_table"):
		_workspace.select_table(_tables[idx])


func _on_id_changed(value: String) -> void:
	if _suppress_signals or _editor == null or _editor.current_table == null:
		return
	var table := _editor.current_table
	_editor.set_table_id(table, value)
	var idx := _tables.find(table)
	if idx >= 0:
		_list.set_item_text(idx, value)
	_update_caption()


func _on_curve_draw() -> void:
	var rect := _curve_view.get_rect()
	var w := rect.size.x
	var h := rect.size.y
	# Background
	_curve_view.draw_rect(Rect2(Vector2.ZERO, Vector2(w, h)), Color(0.10, 0.12, 0.14, 1.0), true)
	# Horizontal gridlines at 0, 0.5, 1.0 with labels.
	var grid := Color(0.30, 0.32, 0.36, 0.7)
	var font := _curve_view.get_theme_default_font()
	var font_size := 11
	for frac: float in [0.0, 0.5, 1.0]:
		var y := h - frac * h
		_curve_view.draw_line(Vector2(0.0, y), Vector2(w, y), grid, 1.0)
		if font != null:
			_curve_view.draw_string(font, Vector2(2.0, clampf(y - 2.0, 10.0, h - 2.0)),
				"%.1f" % frac, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.5, 0.5, 0.55))
	# Axes
	_curve_view.draw_line(Vector2(0.0, h - 1.0), Vector2(w, h - 1.0), Color(0.45, 0.45, 0.5, 1.0), 1.0)
	_curve_view.draw_line(Vector2(1.0, 0.0), Vector2(1.0, h), Color(0.45, 0.45, 0.5, 1.0), 1.0)

	if _editor == null or _editor.current_table == null:
		return
	var data: PackedByteArray = _editor.current_table.get_data()
	if data.size() == 0:
		return
	var prev_pt := Vector2(0.0, h - (float(data[0]) / 255.0) * h)
	for i in range(1, data.size()):
		var x := float(i) / float(data.size() - 1) * w
		var y := h - (float(data[i]) / 255.0) * h
		var pt := Vector2(x, y)
		_curve_view.draw_line(prev_pt, pt, Color(0.85, 0.95, 0.45, 1.0), 1.5)
		prev_pt = pt


# --- Interactive curve editing -----------------------------------------------

func _on_curve_input(event: InputEvent) -> void:
	if _editor == null or _editor.current_table == null:
		return
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			_last_paint_index = -1
			_paint_at(event.position)
		else:
			_last_paint_index = -1
	elif event is InputEventMouseMotion and (event.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0:
		_paint_at(event.position)


func _paint_at(pos: Vector2) -> void:
	var table: NovaParticleTable = _editor.current_table
	var data := _ensure_256(table.get_data())
	var rect := _curve_view.get_rect()
	var w := maxf(rect.size.x, 1.0)
	var h := maxf(rect.size.y, 1.0)
	var index := clampi(int(pos.x / w * 256.0), 0, 255)
	var value := clampi(int(round((1.0 - pos.y / h) * 255.0)), 0, 255)
	if _last_paint_index < 0 or _last_paint_index == index:
		data[index] = value
	else:
		# Interpolate across the dragged span so fast drags paint a smooth line.
		var lo := mini(_last_paint_index, index)
		var hi := maxi(_last_paint_index, index)
		for i in range(lo, hi + 1):
			var frac := float(i - _last_paint_index) / float(index - _last_paint_index)
			data[i] = clampi(int(round(lerpf(float(_last_paint_value), float(value), frac))), 0, 255)
	_last_paint_index = index
	_last_paint_value = value
	_commit_curve(data)


func _on_curve_ramp() -> void:
	if _editor == null or _editor.current_table == null:
		return
	var data := PackedByteArray()
	data.resize(256)
	for i in range(256):
		data[i] = i
	_commit_curve(data)


func _on_curve_flat() -> void:
	_fill_curve(128)


func _on_curve_invert() -> void:
	if _editor == null or _editor.current_table == null:
		return
	var data := _ensure_256(_editor.current_table.get_data())
	for i in range(256):
		data[i] = 255 - data[i]
	_commit_curve(data)


func _fill_curve(value: int) -> void:
	if _editor == null or _editor.current_table == null:
		return
	var data := PackedByteArray()
	data.resize(256)
	for i in range(256):
		data[i] = clampi(value, 0, 255)
	_commit_curve(data)


func _ensure_256(data: PackedByteArray) -> PackedByteArray:
	if data.size() == 256:
		return data
	var out := PackedByteArray()
	out.resize(256)
	for i in range(256):
		out[i] = data[i] if i < data.size() else 0
	return out


func _commit_curve(data: PackedByteArray) -> void:
	_editor.current_table.set_data(data)
	_curve_view.queue_redraw()
	_update_caption()
	_editor.notify_table_changed()
