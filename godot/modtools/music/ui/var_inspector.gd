class_name MusicVarInspector
extends Control

const MusVarNames = preload("res://modtools/music/mus_var_names.gd")

var _director: NovaMusicDirector
# The MU01 chunk's script name (e.g. "menuscript"). Drives the friendly-label
# and control-type lookup. Empty string means rows render the raw VarXX form
# with plain int32 spinboxes (the user-authored / unknown-script case).
var _script_name: String = ""

# var_index -> { control: Control, setter: Callable, value_label: Label,
#                name_label: Label }. Replaces the old positional
# `var_index * 2 + 1` child lookup so rows can be grouped/reordered and use
# heterogeneous controls without breaking incoming-value routing.
var _controls: Dictionary = {}

@onready var _grid: GridContainer = %Grid


func bind_director(d: NovaMusicDirector) -> void:
	_director = d
	_director.variable_changed.connect(_on_var_changed_external)
	_build_rows()


# Called by live_mode after a project loads or when the active script
# changes; rebuilds the rows so labels + control types reflect the new
# script's known var roles.
func set_script_name(script_name: String) -> void:
	if _script_name == script_name:
		return
	_script_name = script_name
	if _director != null:
		_build_rows()


# 17 int32 slots: Var00..Var15 plus the user global Var16 (MUS_GLOBALS_BYTES
# 68 / 4). Known vars for the active script render first (the handful that
# matter), then a separator, then the remaining raw slots. Unknown scripts
# have no known vars, so every slot renders in raw order, identical to the
# pre-grouping behaviour.
func _build_rows() -> void:
	for c in _grid.get_children():
		c.queue_free()
	_controls.clear()
	var known: Array = MusVarNames.known_indices(_script_name)
	var order: Array = known.duplicate()
	for i in range(17):
		if not (i in known):
			order.append(i)
	var known_count: int = known.size()
	var placed: int = 0
	for i in order:
		if known_count > 0 and placed == known_count:
			# Divider between the known group and the raw slots (one cell per
			# column since GridContainer has no row span).
			_grid.add_child(HSeparator.new())
			_grid.add_child(HSeparator.new())
			_grid.add_child(HSeparator.new())
		_add_row(i)
		placed += 1


func _add_row(i: int) -> void:
	var name_label := Label.new()
	name_label.text = MusVarNames.label_for(_script_name, i)
	_grid.add_child(name_label)
	var entry: Dictionary = _make_control(i, MusVarNames.meta_for(_script_name, i))
	entry["name_label"] = name_label
	_grid.add_child(entry["control"])
	_grid.add_child(entry["value_label"])
	_controls[i] = entry
	# Seed from the VM's current value so readouts aren't stuck at 0 before the
	# first variable_changed. get_var returns 0 when the VM isn't running.
	var cur: int = 0
	if _director != null:
		cur = _director.get_var(i)
	(entry["setter"] as Callable).call(cur)


# Builds the column-2 control + column-3 live-value label for one var. Returns
# { control, setter, value_label }. `setter` applies an incoming VM value to
# the control WITHOUT re-emitting (so external updates don't echo back into
# set_var) and refreshes the live-value label.
func _make_control(i: int, meta: Dictionary) -> Dictionary:
	var kind: String = meta.get("kind", "int")
	var value_label := Label.new()
	value_label.custom_minimum_size = Vector2(52, 0)
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value_label.add_theme_color_override("font_color", Color(0.6, 0.8, 1.0))
	match kind:
		"slider":
			var slider := HSlider.new()
			slider.min_value = meta.get("min", 0)
			slider.max_value = meta.get("max", 100)
			slider.step = 1
			slider.custom_minimum_size = Vector2(120, 0)
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			slider.value_changed.connect(func(v):
				if _director != null:
					_director.set_var(i, int(v))
				value_label.text = str(int(v)))
			var setter := func(val):
				slider.set_value_no_signal(float(val))
				value_label.text = str(val)
			return {"control": slider, "setter": setter, "value_label": value_label}
		"bool":
			var cb := CheckBox.new()
			cb.toggled.connect(func(pressed):
				if _director != null:
					_director.set_var(i, 1 if pressed else 0)
				value_label.text = "1" if pressed else "0")
			var setter := func(val):
				cb.set_pressed_no_signal(int(val) != 0)
				value_label.text = str(val)
			return {"control": cb, "setter": setter, "value_label": value_label}
		"enum":
			var ob := OptionButton.new()
			ob.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			var options: Dictionary = meta.get("options", {})
			var ids: Array = options.keys()
			ids.sort()
			for id in ids:
				ob.add_item(String(options[id]))
				ob.set_item_id(ob.item_count - 1, int(id))
			ob.item_selected.connect(func(_idx):
				var sel: int = ob.get_selected_id()
				if _director != null:
					_director.set_var(i, sel)
				value_label.text = str(sel))
			var setter := func(val):
				var iv: int = int(val)
				var found: bool = false
				for k in range(ob.item_count):
					if ob.get_item_id(k) == iv:
						ob.select(k)
						found = true
						break
				if not found:
					ob.select(-1)
				value_label.text = str(val)
			return {"control": ob, "setter": setter, "value_label": value_label}
		_:
			var spin := SpinBox.new()
			spin.min_value = meta.get("min", -2147483648)
			spin.max_value = meta.get("max", 2147483647)
			spin.step = 1
			# min/max are display hints only: the VM can hold (and an author may
			# want to test) any int32, so never let them clamp the true value.
			# Without this, seeding the VM's default 0 into a hinted var like
			# menuscript "Entry" (min 1) would clamp the widget up to 1 and look
			# hardset, mismatching the live-value column.
			spin.allow_lesser = true
			spin.allow_greater = true
			spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			spin.value_changed.connect(func(v):
				if _director != null:
					_director.set_var(i, int(v))
				value_label.text = str(int(v)))
			var setter := func(val):
				spin.set_value_no_signal(float(val))
				value_label.text = str(val)
			return {"control": spin, "setter": setter, "value_label": value_label}


func _on_var_changed_external(var_index: int, value: int) -> void:
	# Route an external VM update to the matching control via its no-signal
	# setter. The globals area is the source of truth; a var_index past the
	# rows we built (or one we don't render) is simply ignored.
	var entry: Dictionary = _controls.get(var_index, {})
	if entry.is_empty():
		return
	(entry["setter"] as Callable).call(value)


# --- Test / introspection helpers --------------------------------------

# The editable control (SpinBox / HSlider / CheckBox / OptionButton) for a var,
# or null if that var has no row.
func get_value_control(var_index: int) -> Control:
	var entry: Dictionary = _controls.get(var_index, {})
	if entry.is_empty():
		return null
	return entry["control"] as Control


# The displayed label text for a var's row (e.g. "MissionActive (Var01)").
func get_row_label_text(var_index: int) -> String:
	var entry: Dictionary = _controls.get(var_index, {})
	if entry.is_empty():
		return ""
	return (entry["name_label"] as Label).text


# Number of var rows currently built (always 17 once rows exist).
func control_count() -> int:
	return _controls.size()
