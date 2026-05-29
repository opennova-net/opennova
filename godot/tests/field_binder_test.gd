extends GutTest

const FieldBinderScript = preload("res://modtools/framework/field_binder.gd")


func test_spin_sync_pushes_value_and_guards_echo() -> void:
	var binder = FieldBinderScript.new()
	var writes := []
	var spin := SpinBox.new()
	add_child_autofree(spin)
	spin.max_value = 100.0
	binder.bind_spin(spin, func(info): return float(info.get("threshold", 0.0)), func(v): writes.append(v))
	binder.sync_from({"threshold": 42.0})
	assert_eq(spin.value, 42.0, "sync_from should push the model value into the control.")
	assert_eq(writes.size(), 0, "sync_from must not echo back through the setter.")


func test_spin_user_edit_calls_setter() -> void:
	var binder = FieldBinderScript.new()
	var writes := []
	var spin := SpinBox.new()
	add_child_autofree(spin)
	spin.max_value = 100.0
	binder.bind_spin(spin, func(info): return float(info.get("threshold", 0.0)), func(v): writes.append(v))
	spin.value = 7.0
	assert_eq(writes, [7.0], "A user edit (outside sync) should call the setter once with the new value.")


func test_checkbox_sync_and_user_toggle() -> void:
	var binder = FieldBinderScript.new()
	var writes := []
	var cb := CheckBox.new()
	add_child_autofree(cb)
	binder.bind_checkbox(cb, func(info): return bool(info.get("on", false)), func(v): writes.append(v))
	binder.sync_from({"on": true})
	assert_true(cb.button_pressed, "sync_from should set the checkbox state.")
	assert_eq(writes.size(), 0, "sync_from must not echo the toggle.")
	cb.button_pressed = false
	assert_eq(writes, [false], "Toggling the checkbox should call the setter.")


func test_color_and_line_bind_push_and_emit() -> void:
	var binder = FieldBinderScript.new()
	var color_writes := []
	var text_writes := []
	var picker := ColorPickerButton.new()
	var line := LineEdit.new()
	add_child_autofree(picker)
	add_child_autofree(line)
	binder.bind_color(picker, func(info): return info.get("col", Color.BLACK), func(v): color_writes.append(v))
	binder.bind_line(line, func(info): return String(info.get("name", "")), func(v): text_writes.append(v))
	binder.sync_from({"col": Color(1, 0, 0, 1), "name": "hello"})
	assert_eq(picker.color, Color(1, 0, 0, 1), "sync_from should push the color.")
	assert_eq(line.text, "hello", "sync_from should push the line text.")
	picker.color_changed.emit(Color(0, 1, 0, 1))
	line.text_submitted.emit("world")
	assert_eq(color_writes, [Color(0, 1, 0, 1)], "color_changed should call the color setter.")
	assert_eq(text_writes, ["world"], "text_submitted should call the line setter.")
