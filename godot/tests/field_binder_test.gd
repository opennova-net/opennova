extends GutTest

const FieldBinderScript = preload("res://engine/ui/field_binder.gd")


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


# Proves bind_link's duck-typed contract is widget-agnostic: any Object with
# set_value/get_value and a value_changed signal binds, not just ResourceRefWidget.
class FakeLinkWidget:
	extends RefCounted
	signal value_changed(value: String)
	var _value := ""

	func set_value(text: String) -> void:
		_value = text  # silent, per contract

	func get_value() -> String:
		return _value

	func user_edit(text: String) -> void:
		_value = text
		value_changed.emit(text)


func test_bind_link_sync_pushes_value_without_echo() -> void:
	var binder = FieldBinderScript.new()
	var writes := []
	var widget := FakeLinkWidget.new()
	binder.bind_link(widget, func(info): return String(info.get("terrain", "")), func(v): writes.append(v))
	binder.sync_from({"terrain": "dvxi5"})
	assert_eq(widget.get_value(), "dvxi5", "sync_from should push the model value into the widget.")
	assert_eq(writes.size(), 0, "sync_from must not echo back through the setter.")


func test_bind_link_user_change_calls_setter_once() -> void:
	var binder = FieldBinderScript.new()
	var writes := []
	var widget := FakeLinkWidget.new()
	binder.bind_link(widget, func(info): return String(info.get("terrain", "")), func(v): writes.append(v))
	widget.user_edit("full_00")
	assert_eq(writes, ["full_00"], "a user change (outside sync) should call the setter once.")


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


func test_option_sync_selects_matching_id_and_guards_echo() -> void:
	var binder = FieldBinderScript.new()
	var writes := []
	var option := OptionButton.new()
	add_child_autofree(option)
	option.add_item("Clear"); option.set_item_id(0, 0)
	option.add_item("Rain"); option.set_item_id(1, 5)
	binder.bind_option(option, func(info): return int(info.get("weather", 0)), func(v): writes.append(v))
	binder.sync_from({"weather": 5})
	assert_eq(option.get_selected_id(), 5, "sync_from should select the item whose id matches the model.")
	assert_eq(writes.size(), 0, "sync_from must not echo back through the setter.")
	option.item_selected.emit(0)
	assert_eq(writes, [0], "A user pick should call the setter with the chosen item id.")


func test_option_sync_surfaces_value_absent_from_choices() -> void:
	# A shipped mission can carry a header enum value outside the editor's curated list. The control
	# must show that value as a row instead of rendering blank (selected = -1).
	var binder = FieldBinderScript.new()
	var option := OptionButton.new()
	add_child_autofree(option)
	option.add_item("Clear"); option.set_item_id(0, 0)
	option.add_item("Rain"); option.set_item_id(1, 5)
	binder.bind_option(option, func(info): return int(info.get("weather", 0)), func(_v): pass)
	binder.sync_from({"weather": 9})
	assert_eq(option.get_selected_id(), 9, "an out-of-range value should be surfaced as a selectable row.")
	assert_eq(option.item_count, 3, "exactly one fallback row should be appended.")
	# Re-syncing must not pile up duplicate fallback rows.
	binder.sync_from({"weather": 11})
	assert_eq(option.get_selected_id(), 11, "the fallback should track the current out-of-range value.")
	assert_eq(option.item_count, 3, "the prior fallback row is replaced, not accumulated.")
	# Returning to an in-range value drops the fallback row entirely.
	binder.sync_from({"weather": 5})
	assert_eq(option.get_selected_id(), 5, "an in-range value selects the real row.")
	assert_eq(option.item_count, 2, "the fallback row is removed once the value is back in range.")


func test_line_commits_on_focus_out() -> void:
	# Leaving a field for another one WITHOUT pressing Enter must still persist the edit. Otherwise the
	# next model `changed` re-syncs this now-unfocused LineEdit back to its stale model value and
	# silently blanks the user's typing (the mission-header "Name blanks when you tab to Designer" bug).
	var binder = FieldBinderScript.new()
	var writes := []
	var line := LineEdit.new()
	add_child_autofree(line)
	binder.bind_line(line, func(info): return String(info.get("name", "")), func(v): writes.append(v))
	line.text = "typed but not submitted"
	line.focus_exited.emit()
	assert_eq(writes, ["typed but not submitted"], "focus-out should commit the current text once.")
	# And a programmatic sync_from (which writes .text on unfocused fields) must not echo a commit.
	binder.sync_from({"name": "from model"})
	assert_eq(line.text, "from model", "sync_from pushes the model value.")
	assert_eq(writes.size(), 1, "sync_from must not commit through the setter.")
