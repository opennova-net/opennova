class_name MusicVarInspector
extends Control

const MusVarNames = preload("res://modtools/music/mus_var_names.gd")

var _director: NovaMusicDirector
# The MU01 chunk's script name (e.g. "menuscript"). Drives the friendly-label
# lookup. Empty string means rows render the raw VarXX form.
var _script_name: String = ""

@onready var _grid: GridContainer = %Grid


func bind_director(d: NovaMusicDirector) -> void:
	_director = d
	_director.variable_changed.connect(_on_var_changed_external)
	_build_rows()


# Called by live_mode after a project loads or when the active script
# changes; rebuilds the rows so the labels reflect the new script's known
# var roles.
func set_script_name(script_name: String) -> void:
	if _script_name == script_name:
		return
	_script_name = script_name
	if _director != null:
		_build_rows()


func _build_rows() -> void:
	for c in _grid.get_children():
		c.queue_free()
	for i in range(16):
		var label := Label.new()
		label.text = MusVarNames.label_for(_script_name, i)
		_grid.add_child(label)
		var spin := SpinBox.new()
		spin.min_value = -2147483648
		spin.max_value = 2147483647
		spin.step = 1
		spin.value_changed.connect(func(v): _director.set_var(i, int(v)))
		spin.set_meta("var_index", i)
		_grid.add_child(spin)


func _on_var_changed_external(var_index: int, value: int) -> void:
	# Use set_value_no_signal to avoid echoing back into set_var.
	var spin := _grid.get_child(var_index * 2 + 1) as SpinBox
	if spin:
		spin.set_value_no_signal(float(value))
