@tool
extends EditorProperty
## One native document field, presented in Godot's regular Inspector layout.

var _session: WorldEditSession
var _field: WorldEditSession.Field
var _slot := 0
var _request_edit: Callable
var _input: Control
var _error: Label
var _updating := false
var _displayed_text := ""


func setup(session: WorldEditSession, field: WorldEditSession.Field, slot: int,
		caption: String, request_edit: Callable) -> void:
	_session = session
	_field = field
	_slot = slot
	_request_edit = request_edit
	label = caption
	tooltip_text = session.get_file_name(field)
	match field:
		WorldEditSession.Field.SKY_HEIGHT, WorldEditSession.Field.FOLIAGE_MATCH:
			var spin := SpinBox.new()
			spin.min_value = 10 if field == WorldEditSession.Field.SKY_HEIGHT else -1
			spin.max_value = 500 if field == WorldEditSession.Field.SKY_HEIGHT else 255
			spin.step = 1
			spin.value_changed.connect(_number_changed)
			spin.editable = session.is_editable()
			_input = spin
		WorldEditSession.Field.FOLIAGE_SHADOW:
			var check := CheckBox.new()
			check.toggled.connect(_commit)
			check.disabled = not session.is_editable()
			_input = check
		_:
			var line := LineEdit.new()
			line.text_submitted.connect(_text_submitted)
			line.focus_exited.connect(_text_finished)
			line.editable = session.is_editable()
			line.placeholder_text = "HH:MM" if field == WorldEditSession.Field.START_TIME else "Asset filename"
			_input = line
	_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_input)
	add_focusable(_input)
	_error = Label.new()
	_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_error.add_theme_color_override("font_color", get_theme_color("error_color", "Editor"))
	add_child(_error)
	set_bottom_editor(_error)
	_error.hide()
	_session.changed.connect(_refresh)
	_refresh()


func _refresh(_changed_session: WorldEditSession = null) -> void:
	_updating = true
	var value: Variant = _session.get_value(_field, _slot)
	if _input is SpinBox:
		(_input as SpinBox).set_value_no_signal(float(value))
	elif _input is CheckBox:
		(_input as CheckBox).set_pressed_no_signal(bool(value))
	else:
		var text := str(value)
		if _field == WorldEditSession.Field.START_TIME:
			var minutes := roundi(MissionEnvironment.hhmm_to_minute_of_day(
					MissionEnvironment.mission_start_time_hhmm(int(value)))) % 1440
			text = "%02d:%02d" % [minutes / 60, minutes % 60]
		(_input as LineEdit).text = text
		_displayed_text = text
	_error.hide()
	_updating = false


func _number_changed(value: float) -> void:
	_commit(int(value) if _field == WorldEditSession.Field.FOLIAGE_MATCH else value)


func _text_submitted(_text: String) -> void:
	_text_finished()


func _text_finished() -> void:
	var text := (_input as LineEdit).text.strip_edges()
	if text == _displayed_text:
		return
	if _field == WorldEditSession.Field.START_TIME:
		var parts := text.split(":")
		if parts.size() != 2 or not parts[0].is_valid_int() or not parts[1].is_valid_int() \
				or int(parts[0]) < 0 or int(parts[0]) > 23 or int(parts[1]) < 0 or int(parts[1]) > 59:
			_show_error("Use a time between 00:00 and 23:59.")
			return
		_commit(roundi((int(parts[0]) + int(parts[1]) / 60.0) * 256.0))
	else:
		_commit(text)


func _commit(value: Variant) -> void:
	if _updating:
		return
	var reason: String = _request_edit.call(_session, _field, value, _slot)
	if not reason.is_empty():
		_show_error(reason)
	else:
		_refresh()


func _show_error(reason: String) -> void:
	_error.text = reason
	_error.show()
