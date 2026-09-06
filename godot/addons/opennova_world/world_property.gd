@tool
extends EditorProperty
## One native document field, presented in Godot's regular Inspector layout.
## The WorldField spec chooses the widget, its range and its placeholder.

signal pending_changed

var _session: WorldEditSession
var _spec: WorldField
var _slot := 0
var _request_edit: Callable
var _input: Control
var _error: Label
var _updating := false
var _displayed_text := ""


func setup(session: WorldEditSession, field: WorldField.Id, slot: int, request_edit: Callable) -> void:
	_session = session
	_spec = WorldField.spec(field)
	_slot = slot
	_request_edit = request_edit
	label = _spec.label
	tooltip_text = session.get_file_name(field)
	match _spec.widget:
		WorldField.Widget.INT_SPIN, WorldField.Widget.FLOAT_SPIN:
			var spin := SpinBox.new()
			spin.min_value = _spec.min_value
			spin.max_value = _spec.max_value
			spin.step = _spec.step
			spin.value_changed.connect(_number_changed)
			spin.get_line_edit().text_changed.connect(_text_changed)
			spin.editable = session.is_editable()
			_input = spin
		WorldField.Widget.CHECK:
			var check := CheckBox.new()
			check.toggled.connect(_commit)
			check.disabled = not session.is_editable()
			_input = check
		_:
			var line := LineEdit.new()
			line.text_submitted.connect(_text_submitted)
			line.text_changed.connect(_text_changed)
			line.focus_exited.connect(_text_finished)
			line.editable = session.is_editable()
			line.placeholder_text = _spec.placeholder
			_input = line
	_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_input)
	add_focusable(_input)
	_error = Label.new()
	_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_error)
	set_bottom_editor(_error)
	_error.hide()
	_session.changed.connect(_refresh)
	_refresh()


func _notification(what: int) -> void:
	# Editor theme colors resolve only once this control is in the tree.
	if what == NOTIFICATION_THEME_CHANGED and _error != null:
		_error.add_theme_color_override("font_color", get_theme_color("error_color", "Editor"))


func get_pending_file() -> String:
	var text := _displayed_text
	if _input is LineEdit:
		text = (_input as LineEdit).text.strip_edges()
	elif _input is SpinBox:
		text = (_input as SpinBox).get_line_edit().text.strip_edges()
	return _session.get_file_name(_spec.id) if text != _displayed_text else ""


func flush_pending_edit() -> String:
	if _input is LineEdit:
		_text_finished()
	elif _input is SpinBox:
		(_input as SpinBox).apply()
	return _error.text if _error.visible else ""


func _text_changed(_text: String) -> void:
	if not _updating:
		pending_changed.emit()


func _refresh(_changed_session: WorldEditSession = null) -> void:
	_updating = true
	var value: Variant = _session.get_value(_spec.id, _slot)
	if _input is SpinBox:
		var spin := _input as SpinBox
		spin.set_value_no_signal(float(value))
		# SpinBox normally updates its text on draw. Synchronize it now so
		# save/close hooks compare against the newly displayed integer value.
		_displayed_text = str(roundi(spin.value))
		spin.get_line_edit().text = _displayed_text
	elif _input is CheckBox:
		(_input as CheckBox).set_pressed_no_signal(bool(value))
	else:
		var text := str(value)
		if _spec.widget == WorldField.Widget.TIME_TEXT:
			var minutes := roundi(MissionEnvironment.hhmm_to_minute_of_day(
					MissionEnvironment.mission_start_time_hhmm(int(value)))) \
					% (WorldField.HOURS_PER_DAY * WorldField.MINUTES_PER_HOUR)
			text = "%02d:%02d" % [minutes / WorldField.MINUTES_PER_HOUR, minutes % WorldField.MINUTES_PER_HOUR]
		(_input as LineEdit).text = text
		_displayed_text = text
	_error.hide()
	_updating = false


func _number_changed(value: float) -> void:
	_commit(int(value) if _spec.widget == WorldField.Widget.INT_SPIN else value)


func _text_submitted(_text: String) -> void:
	_text_finished()


func _text_finished() -> void:
	var text := (_input as LineEdit).text.strip_edges()
	if text == _displayed_text:
		_error.hide()
		return
	if _spec.widget == WorldField.Widget.TIME_TEXT:
		var parts := text.split(":")
		if parts.size() != 2 or not parts[0].is_valid_int() or not parts[1].is_valid_int():
			_show_error("Use a time between 00:00 and 23:59.")
			return
		var hours := int(parts[0])
		var minutes := int(parts[1])
		if hours < 0 or hours >= WorldField.HOURS_PER_DAY or minutes < 0 or minutes >= WorldField.MINUTES_PER_HOUR:
			_show_error("Use a time between 00:00 and 23:59.")
			return
		_commit(roundi((hours + float(minutes) / WorldField.MINUTES_PER_HOUR) * WorldField.START_TIME_UNITS_PER_HOUR))
	else:
		_commit(text)


func _commit(value: Variant) -> void:
	if _updating:
		return
	var reason: String = _request_edit.call(_session, _spec.id, value, _slot)
	if not reason.is_empty():
		_show_error(reason)
	else:
		_refresh()


func _show_error(reason: String) -> void:
	_error.text = reason
	_error.show()
