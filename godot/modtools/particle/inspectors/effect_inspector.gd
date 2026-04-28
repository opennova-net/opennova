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


func _ready() -> void:
	_list.item_selected.connect(_on_item_selected)
	_id_edit.text_changed.connect(_on_id_changed)
	_pdefs_edit.text_changed.connect(_on_pdefs_changed)
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
	_editor.current_effect.id = new_text
	_editor.mark_dirty()
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
	_editor.current_effect.pdefs = lines
	_editor.mark_dirty()
