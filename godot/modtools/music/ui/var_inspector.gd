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
	# 17 int32 slots: Var00..Var15 plus the user global Var16
	# (MUS_GLOBALS_BYTES 68 / 4). The VM can write Var16, so it needs a row.
	for i in range(17):
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
	# Use set_value_no_signal to avoid echoing back into set_var. Guard the
	# child index: the VM may report a var_index past the rows we built (the
	# globals area is the source of truth, not this view).
	var child_index := var_index * 2 + 1
	if child_index < 0 or child_index >= _grid.get_child_count():
		return
	var spin := _grid.get_child(child_index) as SpinBox
	if spin:
		spin.set_value_no_signal(float(value))
