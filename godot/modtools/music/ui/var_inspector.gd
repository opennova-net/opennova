class_name MusicVarInspector
extends Control

# A variable was renamed (the profile sidecar changed): owners refresh every
# other surface that shows var names (blueprint bodies, pickers, event log).
signal names_changed

const MusVarNames = preload("res://modtools/music/mus_var_names.gd")

var _director: NovaMusicDirector
# The MU01 chunk's script name (e.g. "menuscript"). Drives the friendly-label
# and control-type lookup. Empty string means rows render the raw VarXX form
# with plain int32 spinboxes (the user-authored / unknown-script case).
var _script_name: String = ""
var _profile_path: String = ""

# var_index -> { control: Control, setter: Callable, value_label: Label,
#                name_label: Label }. Replaces the old positional
# `var_index * 2 + 1` child lookup so rows can be grouped/reordered and use
# heterogeneous controls without breaking incoming-value routing.
var _controls: Dictionary = {}

@onready var _grid: GridContainer = %Grid


func bind_director(d: NovaMusicDirector) -> void:
	if _director != null and _director.variable_changed.is_connected(_on_var_changed_external):
		_director.variable_changed.disconnect(_on_var_changed_external)
	_director = d
	if _director != null and not _director.variable_changed.is_connected(_on_var_changed_external):
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


func set_profile_path(profile_path: String) -> void:
	if _profile_path == profile_path:
		return
	_profile_path = profile_path
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
	var known: Array = MusVarNames.known_indices(_script_name, _profile_path)
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
	var meta := MusVarNames.meta_for(_script_name, i, _profile_path)
	var name_label := Label.new()
	name_label.text = MusVarNames.label_for(_script_name, i, _profile_path)
	var tip := _tooltip_for(i, name_label.text, meta)
	name_label.tooltip_text = tip
	# Name cell: label + a ✎ rename affordance (writes the display name to the
	# project's profile sidecar; the stored script keeps its VarXX tokens).
	var name_cell := HBoxContainer.new()
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_cell.add_child(name_label)
	var rename := Button.new()
	rename.text = "✎"
	rename.flat = true
	rename.focus_mode = Control.FOCUS_NONE
	if _profile_path == "":
		rename.disabled = true
		rename.tooltip_text = "Save the project first to name its variables."
	else:
		rename.tooltip_text = "Give this variable a friendly name (display only; the file keeps Var%02d)." % i
		rename.pressed.connect(func(): _open_rename_popup(i))
	name_cell.add_child(rename)
	_grid.add_child(name_cell)
	var entry: Dictionary = _make_control(i, meta)
	entry["name_label"] = name_label
	(entry["control"] as Control).tooltip_text = tip
	(entry["value_label"] as Control).tooltip_text = tip
	_grid.add_child(entry["control"])
	_grid.add_child(entry["value_label"])
	_controls[i] = entry
	# Seed from the VM's current value so readouts aren't stuck at 0 before the
	# first variable_changed. get_var returns 0 when the VM isn't running.
	var cur: int = 0
	if _director != null:
		cur = _director.get_var(i)
	(entry["setter"] as Callable).call(cur)


func _tooltip_for(var_index: int, label_text: String, meta: Dictionary) -> String:
	var token := "Var%02d" % var_index
	var kind := String(meta.get("kind", "int"))
	match kind:
		"slider":
			return "%s. Slider range: %d..%d. Adjusting writes %s in the VM." % [
				label_text,
				int(meta.get("min", 0)),
				int(meta.get("max", 100)),
				token,
			]
		"bool":
			return "%s. Checkbox: off=0, on=1. Adjusting writes %s in the VM." % [label_text, token]
		"enum":
			return "%s. Pick a known value; unknown live values show as the raw number. Adjusting writes %s in the VM." % [label_text, token]
		_:
			if meta.has("min") or meta.has("max"):
				return "%s. Number range hint: %d..%d. Values outside the hint are still allowed. Adjusting writes %s in the VM." % [
					label_text,
					int(meta.get("min", -2147483648)),
					int(meta.get("max", 2147483647)),
					token,
				]
			return "%s. Raw int32 slot. Adjusting writes %s in the VM." % [label_text, token]


# Builds the column-2 control + column-3 live-value label for one var. Returns
# { control, setter, value_label }. `setter` applies an incoming VM value to
# the control WITHOUT re-emitting (so external updates don't echo back into
# set_var) and refreshes the live-value label.
#
# The column-3 readout is only populated where the control doesn't already show
# the number: a slider has no numeric display, and an enum shows a label ("MP")
# not the stored int. A SpinBox and a CheckBox already read out their own value,
# so duplicating it there just printed every dial's value twice (the "0 ... 0"
# noise across the raw-slot rows). For those the label stays blank but present,
# so the GridContainer's three columns still line up.
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
			# The checkbox itself reads out on/off, so no duplicate column-3 number.
			var cb := CheckBox.new()
			cb.toggled.connect(func(pressed):
				if _director != null:
					_director.set_var(i, 1 if pressed else 0))
			var setter := func(val):
				cb.set_pressed_no_signal(int(val) != 0)
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
			# The spinbox shows its own value, so no duplicate column-3 number.
			spin.value_changed.connect(func(v):
				if _director != null:
					_director.set_var(i, int(v)))
			var setter := func(val):
				spin.set_value_no_signal(float(val))
			return {"control": spin, "setter": setter, "value_label": value_label}


func _on_var_changed_external(var_index: int, value: int) -> void:
	# Route an external VM update to the matching control via its no-signal
	# setter. The globals area is the source of truth; a var_index past the
	# rows we built (or one we don't render) is simply ignored.
	var entry: Dictionary = _controls.get(var_index, {})
	if entry.is_empty():
		return
	(entry["setter"] as Callable).call(value)


func refresh_from_director() -> void:
	if _director == null:
		return
	for var_index in _controls.keys():
		var entry: Dictionary = _controls[var_index]
		(entry["setter"] as Callable).call(_director.get_var(int(var_index)))


# One-line rename popup: the new display name (empty clears the custom name and
# falls back to the built-in / raw form). Writes through MusVarNames.set_label,
# rebuilds the rows, and announces names_changed for the mount's other surfaces.
func _open_rename_popup(var_index: int) -> void:
	var dlg := ConfirmationDialog.new()
	dlg.title = "Name Var%02d" % var_index
	dlg.min_size = Vector2i(320, 110)
	var box := VBoxContainer.new()
	dlg.add_child(box)
	var lbl := Label.new()
	lbl.text = "Friendly name (leave empty to clear):"
	box.add_child(lbl)
	var edit := LineEdit.new()
	edit.name = "VarNameEdit"
	edit.max_length = 24
	# Prefill with the current label's short form so an existing name edits
	# instead of retyping ("Speed (Var05)" -> "Speed"; raw "Var05" -> empty).
	var current := MusVarNames.label_for(_script_name, var_index, _profile_path)
	var suffix := " (Var%02d)" % var_index
	if current.ends_with(suffix):
		edit.text = current.substr(0, current.length() - suffix.length())
	box.add_child(edit)
	add_child(dlg)
	dlg.confirmed.connect(func():
		_apply_rename(var_index, edit.text)
		dlg.queue_free())
	dlg.canceled.connect(dlg.queue_free)
	dlg.close_requested.connect(dlg.queue_free)
	dlg.popup_centered()
	edit.select_all()
	edit.grab_focus()


func _apply_rename(var_index: int, label: String) -> void:
	if MusVarNames.set_label(_profile_path, _script_name, var_index, label) != OK:
		return
	_build_rows()
	names_changed.emit()


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
