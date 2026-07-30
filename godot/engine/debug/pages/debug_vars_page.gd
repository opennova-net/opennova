class_name DebugVarsPage
extends NovaDebugPage
## The V/G/M script-variable banks with the changed-only filter and the
## (lockable) live-edit toggle. Rows rebuild only when the visible set or the
## writes mode changes; steady-state refreshes update values in place.

var _nonzero_check: CheckBox
var _writes_check: CheckButton
var _vars_rows: VBoxContainer
# (bank, index) key -> the row's value Control, so steady-state refreshes
# update text in place instead of rebuilding ~800 rows.
var _var_controls: Dictionary = {}
var _var_rows_signature := ""


func page_id() -> StringName:
	return &"Vars"


func page_category() -> StringName:
	return CATEGORY_SIM


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_nonzero_check = CheckBox.new()
	_nonzero_check.name = "VarsNonzero"
	_nonzero_check.text = "Show changed values only"
	_nonzero_check.button_pressed = true
	_nonzero_check.toggled.connect(_on_vars_filter_toggled)
	add_child(_nonzero_check)

	_writes_check = CheckButton.new()
	_writes_check.name = "VarsAllowWrites"
	_writes_check.text = "Allow edits"
	_writes_check.tooltip_text = "Editing changes the LIVE mission (V values only)."
	_writes_check.button_pressed = false
	_writes_check.toggled.connect(_on_vars_filter_toggled)
	add_child(_writes_check)

	var scroll := ScrollContainer.new()
	scroll.name = "VarsScroll"
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)

	_vars_rows = VBoxContainer.new()
	_vars_rows.name = "VarsRows"
	_vars_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_vars_rows)


func refresh() -> void:
	var sim := _ctx.sim()
	if sim == null:
		_clear_live()
		return
	var write_state: NovaDebugControlState = _ctx.session.get_control_state(
			&"set_mission_variable") if _ctx.session != null else null
	var policy_writable := write_state != null \
			and write_state.available and write_state.writable
	_writes_check.disabled = not policy_writable
	if _writes_check.disabled:
		_writes_check.set_pressed_no_signal(false)
	var policy_reason := write_state.reason if write_state != null else ""
	_writes_check.tooltip_text = policy_reason if not policy_reason.is_empty() \
			else "Editing changes the LIVE mission (V values only)."
	var banks := [
		["V", sim.get_mission_variables_snapshot(), true],
		["G", sim.get_global_variables_snapshot(), false],
		["M", sim.get_music_variables_snapshot(), false],
	]
	var nonzero_only: bool = _nonzero_check.button_pressed
	var writable: bool = _writes_check.button_pressed \
			and policy_writable

	# Decide what should be visible, then rebuild only when that set (or the
	# writes mode) changed; otherwise update values in place.
	var desired: Array = []
	for bank in banks:
		var values: PackedInt32Array = bank[1]
		for i in range(values.size()):
			if nonzero_only and values[i] == 0:
				continue
			desired.append([String(bank[0]), i, values[i], bool(bank[2])])
	var signature := "%d|%s|%s" % [desired.size(), str(nonzero_only), str(writable)]
	for entry in desired:
		signature += "|%s%d" % [entry[0], entry[1]]

	# Never rebuild out from under an edit in progress: with the changed-only
	# filter on a RUNNING mission, vars flip zero<->nonzero routinely, and the
	# rebuild would drop focus and in-flight text. The stale set survives one
	# refresh cycle; the rebuild lands after the field blurs.
	if signature != _var_rows_signature and _any_var_edit_focused():
		return

	if signature != _var_rows_signature:
		_var_rows_signature = signature
		_var_controls.clear()
		for child in _vars_rows.get_children():
			# Detach before queue_free so the dying rows release their names
			# immediately (replacement rows reuse them) and never shadow
			# lookups; full free stays deferred because a rebuild can be
			# triggered from a row's own LineEdit signal.
			_vars_rows.remove_child(child)
			child.queue_free()
		if desired.is_empty():
			var empty := Label.new()
			empty.name = "VarsEmpty"
			empty.text = "No values set yet."
			_vars_rows.add_child(empty)
		for entry in desired:
			_add_var_row(String(entry[0]), int(entry[1]), int(entry[2]),
					writable and bool(entry[3]))

	for entry in desired:
		var key := "%s%d" % [entry[0], entry[1]]
		var control: Control = _var_controls.get(key)
		if control is LineEdit:
			var edit := control as LineEdit
			if not edit.has_focus():
				edit.text = str(int(entry[2]))
		elif control is Label:
			(control as Label).text = str(int(entry[2]))


func _clear_live() -> void:
	_writes_check.set_pressed_no_signal(false)
	_writes_check.disabled = true
	if _var_rows_signature != "":
		_var_rows_signature = ""
		_var_controls.clear()
		for child in _vars_rows.get_children():
			_vars_rows.remove_child(child)
			child.queue_free()


func _any_var_edit_focused() -> bool:
	for control in _var_controls.values():
		if control is LineEdit and (control as LineEdit).has_focus():
			return true
	return false


func _add_var_row(bank: String, index: int, value: int, writable: bool) -> void:
	var row := HBoxContainer.new()
	row.name = "VarRow_%s%d" % [bank, index]
	var name_label := Label.new()
	name_label.text = "%s%d" % [bank, index]
	name_label.custom_minimum_size = Vector2(64, 0)
	row.add_child(name_label)
	if writable:
		var edit := LineEdit.new()
		edit.name = "VarEdit_%s%d" % [bank, index]
		edit.text = str(value)
		edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		edit.text_submitted.connect(_on_var_submitted.bind(index))
		row.add_child(edit)
		_var_controls["%s%d" % [bank, index]] = edit
	else:
		var value_label := Label.new()
		value_label.name = "VarValue_%s%d" % [bank, index]
		value_label.text = str(value)
		row.add_child(value_label)
		_var_controls["%s%d" % [bank, index]] = value_label
	_vars_rows.add_child(row)


func _on_vars_filter_toggled(_pressed: bool) -> void:
	if _ctx.request_refresh.is_valid():
		_ctx.request_refresh.call()


func _on_var_submitted(text: String, index: int) -> void:
	if not _writes_check.button_pressed:
		return
	if _ctx.session != null and text.is_valid_int():
		_ctx.session.invoke_control(
				&"set_mission_variable", [index, int(text)])
		if _ctx.request_refresh.is_valid():
			_ctx.request_refresh.call()
