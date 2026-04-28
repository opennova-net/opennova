class_name ParticleDefInspector
extends Control

## Lists [particledef] entries with a Godot inspector embed for the selected
## particle. Selection feeds the live preview viewport.

@onready var _list: ItemList = %ParticleList
@onready var _empty_label: Label = %EmptyLabel
@onready var _detail_label: Label = %DetailLabel
@onready var _id_edit: LineEdit = %IdEdit
@onready var _flags_edit: LineEdit = %FlagsEdit
@onready var _move_edit: LineEdit = %MoveEdit
@onready var _emit_dur_spin: SpinBox = %EmitDurSpin
@onready var _emit_rate_spin: SpinBox = %EmitRateSpin
@onready var _emit_burst_spin: SpinBox = %EmitBurstSpin
@onready var _gravity_spin: SpinBox = %GravitySpin
@onready var _drag_spin: SpinBox = %DragSpin
@onready var _scale_spin: SpinBox = %ScaleSpin
@onready var _restart_button: Button = %RestartButton

var _editor: ParticleEditor
var _workspace
var _particles: Array = []
var _suppress_signals := false


func _ready() -> void:
	_list.item_selected.connect(_on_item_selected)
	_id_edit.text_changed.connect(_on_id_changed)
	_flags_edit.text_changed.connect(_on_flags_changed)
	_move_edit.text_changed.connect(_on_move_changed)
	_emit_dur_spin.value_changed.connect(_on_emit_dur_changed)
	_emit_rate_spin.value_changed.connect(_on_emit_rate_changed)
	_emit_burst_spin.value_changed.connect(_on_emit_burst_changed)
	_gravity_spin.value_changed.connect(_on_gravity_changed)
	_drag_spin.value_changed.connect(_on_drag_changed)
	_scale_spin.value_changed.connect(_on_scale_changed)
	_restart_button.pressed.connect(_on_restart_pressed)
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
	_particles.clear()
	_list.clear()
	if _editor != null and _editor.particle_file != null:
		var src: Array = _editor.particle_file.particles
		for entry in src:
			var p: NovaParticleDef = entry
			if p != null:
				_particles.append(p)
				_list.add_item(p.id)
	_refresh_selection()
	_empty_label.visible = _particles.is_empty()
	_set_form_enabled(not _particles.is_empty())


func _set_form_enabled(enabled: bool) -> void:
	_id_edit.editable = enabled
	_flags_edit.editable = enabled
	_move_edit.editable = enabled
	_emit_dur_spin.editable = enabled
	_emit_rate_spin.editable = enabled
	_emit_burst_spin.editable = enabled
	_gravity_spin.editable = enabled
	_drag_spin.editable = enabled
	_scale_spin.editable = enabled
	_restart_button.disabled = not enabled


func _refresh_selection() -> void:
	if _editor == null or _editor.current_particle == null:
		_clear_form()
		_list.deselect_all()
		_detail_label.text = "(no particle selected)"
		return
	var p := _editor.current_particle
	var idx := _particles.find(p)
	if idx >= 0:
		_list.select(idx)
	_detail_label.text = "Selected: %s" % p.id
	_suppress_signals = true
	_id_edit.text = p.id
	_flags_edit.text = p.flags_raw
	_move_edit.text = p.move_raw
	_emit_dur_spin.value = p.emit_dur
	_emit_rate_spin.value = p.emit_rate
	_emit_burst_spin.value = p.emit_burst
	_gravity_spin.value = p.gravity
	_drag_spin.value = p.drag
	_scale_spin.value = p.scale_value
	_suppress_signals = false


func _clear_form() -> void:
	_suppress_signals = true
	_id_edit.text = ""
	_flags_edit.text = ""
	_move_edit.text = ""
	_emit_dur_spin.value = 0.0
	_emit_rate_spin.value = 0.0
	_emit_burst_spin.value = 1
	_gravity_spin.value = 0.0
	_drag_spin.value = 0.0
	_scale_spin.value = 0.0
	_suppress_signals = false


func _on_item_selected(idx: int) -> void:
	if idx < 0 or idx >= _particles.size():
		return
	if _workspace != null and _workspace.has_method("select_particle"):
		_workspace.select_particle(_particles[idx])


func _on_id_changed(text: String) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.id = text
	_editor.mark_dirty()
	var idx := _particles.find(_editor.current_particle)
	if idx >= 0:
		_list.set_item_text(idx, text)


func _on_flags_changed(text: String) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.flags_raw = text
	_editor.mark_dirty()


func _on_move_changed(text: String) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.move_raw = text
	_editor.mark_dirty()


func _on_emit_dur_changed(value: float) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.emit_dur = value
	_editor.mark_dirty()


func _on_emit_rate_changed(value: float) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.emit_rate = value
	_editor.mark_dirty()


func _on_emit_burst_changed(value: float) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.emit_burst = int(value)
	_editor.mark_dirty()


func _on_gravity_changed(value: float) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.gravity = value
	_editor.mark_dirty()


func _on_drag_changed(value: float) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.drag = value
	_editor.mark_dirty()


func _on_scale_changed(value: float) -> void:
	if _suppress_signals or _editor == null or _editor.current_particle == null:
		return
	_editor.current_particle.scale_value = value
	_editor.mark_dirty()


func _on_restart_pressed() -> void:
	# Tell the workspace to re-spawn the preview emitter so edits take effect
	# from frame zero.
	if _workspace == null:
		return
	if _workspace.has_method("select_particle") and _editor != null and _editor.current_particle != null:
		_workspace.select_particle(_editor.current_particle)
