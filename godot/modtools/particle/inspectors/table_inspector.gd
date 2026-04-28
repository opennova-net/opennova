class_name ParticleTableInspector
extends Control

## Lists [tabledef] entries; selecting one renders the 32×8 (256-byte logical
## curve) as a line graph. Read-only for MVP — editing curves comes later.

@onready var _list: ItemList = %TableList
@onready var _empty_label: Label = %EmptyLabel
@onready var _curve_view: Control = %CurveView
@onready var _curve_caption: Label = %CurveCaption

var _editor: ParticleEditor
var _workspace
var _tables: Array = []


func _ready() -> void:
	_list.item_selected.connect(_on_item_selected)
	_curve_view.draw.connect(_on_curve_draw)
	_refresh()


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


func _refresh_selection() -> void:
	if _editor == null or _editor.current_table == null:
		_curve_caption.text = "(no table selected)"
		_curve_view.queue_redraw()
		return
	var idx := _tables.find(_editor.current_table)
	if idx >= 0:
		_list.select(idx)
	_curve_caption.text = "%s — 32 rows × 8 cols (256-byte LUT)" % _editor.current_table.id
	_curve_view.queue_redraw()


func _on_item_selected(idx: int) -> void:
	if idx < 0 or idx >= _tables.size():
		return
	if _workspace != null and _workspace.has_method("select_table"):
		_workspace.select_table(_tables[idx])


func _on_curve_draw() -> void:
	var rect := _curve_view.get_rect()
	var w := rect.size.x
	var h := rect.size.y
	# Background
	_curve_view.draw_rect(Rect2(Vector2.ZERO, Vector2(w, h)), Color(0.10, 0.12, 0.14, 1.0), true)
	# Axes
	_curve_view.draw_line(Vector2(0.0, h - 1.0), Vector2(w, h - 1.0), Color(0.45, 0.45, 0.5, 1.0), 1.0)
	_curve_view.draw_line(Vector2(1.0, 0.0), Vector2(1.0, h), Color(0.45, 0.45, 0.5, 1.0), 1.0)

	if _editor == null or _editor.current_table == null:
		return
	var data: PackedByteArray = _editor.current_table.data
	if data.size() == 0:
		return
	var prev_pt := Vector2(0.0, h - (float(data[0]) / 255.0) * h)
	for i in range(1, data.size()):
		var x := float(i) / float(data.size() - 1) * w
		var y := h - (float(data[i]) / 255.0) * h
		var pt := Vector2(x, y)
		_curve_view.draw_line(prev_pt, pt, Color(0.85, 0.95, 0.45, 1.0), 1.5)
		prev_pt = pt
