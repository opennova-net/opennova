class_name ParticleEffectInspector
extends Control

## Lists [effectdef] entries; selecting one feeds it back to the workspace
## which spawns one preview emitter per referenced pdef.

@onready var _list: ItemList = %EffectList
@onready var _id_edit: LineEdit = %IdEdit
@onready var _pdefs_edit: TextEdit = %PdefsEdit
@onready var _empty_label: Label = %EmptyLabel

var _editor: ParticleEditor
var _workspace
var _effects: Array = []
var _suppress_signals := false
var _dup_button: Button
var _del_button: Button


func _ready() -> void:
	_build_toolbar()
	_list.item_selected.connect(_on_item_selected)
	_id_edit.text_changed.connect(_on_id_changed)
	_pdefs_edit.text_changed.connect(_on_pdefs_changed)
	_refresh()


func _build_toolbar() -> void:
	var toolbar := HBoxContainer.new()
	add_child(toolbar)
	move_child(toolbar, _list.get_index())  # place just above the list
	var add_btn := Button.new()
	add_btn.text = "+ Add"
	add_btn.tooltip_text = "Add a new effect"
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
	if _workspace != null and _workspace.has_method("add_effect"):
		_workspace.add_effect()


func _on_duplicate_pressed() -> void:
	if _editor == null or _editor.current_effect == null:
		return
	if _workspace != null and _workspace.has_method("duplicate_effect"):
		_workspace.duplicate_effect(_editor.current_effect)


func _on_delete_pressed() -> void:
	if _editor == null or _editor.current_effect == null:
		return
	if _workspace != null and _workspace.has_method("remove_effect"):
		_workspace.remove_effect(_editor.current_effect)


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
	_effects.clear()
	_list.clear()
	if _editor != null and _editor.particle_file != null:
		var src: Array = _editor.particle_file.effects
		for entry in src:
			var effect: NovaParticleEffect = entry
			if effect != null:
				_effects.append(effect)
				_list.add_item(effect.id)
	_refresh_selection()
	_empty_label.visible = _effects.is_empty()
	_id_edit.editable = not _effects.is_empty()
	_pdefs_edit.editable = not _effects.is_empty()
	var has_selection := _editor != null and _editor.current_effect != null
	if _dup_button != null:
		_dup_button.disabled = not has_selection
	if _del_button != null:
		_del_button.disabled = not has_selection


func _refresh_selection() -> void:
	if _editor == null or _editor.current_effect == null:
		_id_edit.text = ""
		_pdefs_edit.text = ""
		_list.deselect_all()
		return
	var idx := _effects.find(_editor.current_effect)
	if idx >= 0:
		_list.select(idx)
	_suppress_signals = true
	_id_edit.text = _editor.current_effect.id
	_pdefs_edit.text = "\n".join(Array(_editor.current_effect.pdefs))
	_suppress_signals = false


func _on_item_selected(idx: int) -> void:
	if idx < 0 or idx >= _effects.size():
		return
	if _workspace != null and _workspace.has_method("select_effect"):
		_workspace.select_effect(_effects[idx])


func _on_id_changed(new_text: String) -> void:
	if _suppress_signals or _editor == null or _editor.current_effect == null:
		return
	_editor.set_effect_id(_editor.current_effect, new_text)
	# Keep list label in sync.
	var idx := _effects.find(_editor.current_effect)
	if idx >= 0:
		_list.set_item_text(idx, new_text)


func _on_pdefs_changed() -> void:
	if _suppress_signals or _editor == null or _editor.current_effect == null:
		return
	var lines: PackedStringArray = PackedStringArray()
	for raw_line in _pdefs_edit.text.split("\n"):
		var trimmed: String = raw_line.strip_edges()
		if not trimmed.is_empty():
			lines.append(trimmed)
	_editor.set_effect_pdefs(_editor.current_effect, lines)
