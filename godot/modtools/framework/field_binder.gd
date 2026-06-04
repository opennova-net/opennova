class_name FieldBinder
extends RefCounted

## Declarative inspector field binding. Each bind_*(control, getter, setter) pairs
## a control with a getter(info_dict)->value (read from the model snapshot) and a
## setter(value)->void (write to the model). The change signal is auto-wired and
## guarded by an internal SyncGuard, so sync_from(info) can push fresh model
## values into every control without echoing back through the setters. Replaces
## the per-field "build control + guard.active check + connect lambda + manual
## sync read-back" boilerplate. Setters route through the model (e.g.
## NovaObjectData.set_light_field) unchanged.

var _guard := SyncGuard.new()
var _bindings: Array = []


func bind_spin(spin: SpinBox, getter: Callable, setter: Callable) -> SpinBox:
	# Skip a spin whose inner LineEdit is focused (and only write a changed value): sync_from fires
	# on every model `changed`, including one that lands while the user is mid-typing (e.g. an undo),
	# and a blind `.value =` would clobber the in-flight keystroke and move the caret.
	_bindings.append(func(info):
		var v: float = getter.call(info)
		if spin.value != v and not spin.get_line_edit().has_focus():
			spin.value = v)
	spin.value_changed.connect(func(value: float):
		if not _guard.active:
			setter.call(value))
	return spin


func bind_checkbox(checkbox: CheckBox, getter: Callable, setter: Callable) -> CheckBox:
	_bindings.append(func(info): checkbox.button_pressed = getter.call(info))
	checkbox.toggled.connect(func(value: bool):
		if not _guard.active:
			setter.call(value))
	return checkbox


func bind_color(picker: ColorPickerButton, getter: Callable, setter: Callable) -> ColorPickerButton:
	_bindings.append(func(info): picker.color = getter.call(info))
	picker.color_changed.connect(func(value: Color):
		if not _guard.active:
			setter.call(value))
	return picker


func bind_line(line: LineEdit, getter: Callable, setter: Callable) -> LineEdit:
	# Skip a focused LineEdit (and only write changed text): sync_from fires on every model `changed`,
	# including one that lands while the user is typing (e.g. an undo), and a blind `.text =` would
	# overwrite the half-typed value and reset the caret. The commit-on-Enter/focus-out path reconciles.
	_bindings.append(func(info):
		var v: String = getter.call(info)
		if line.text != v and not line.has_focus():
			line.text = v)
	# Commit on BOTH Enter (text_submitted) and focus-out. Focus-out is essential: leaving a field for
	# another one WITHOUT pressing Enter must persist the edit, otherwise the next model `changed`
	# (e.g. from committing a sibling field) re-syncs this now-unfocused LineEdit back to its stale
	# model value and silently blanks what the user just typed. The guard suppresses any echo during a
	# programmatic sync_from; model-side dedupe makes a no-op focus-out commit harmless.
	line.text_submitted.connect(func(value: String):
		if not _guard.active:
			setter.call(value))
	line.focus_exited.connect(func():
		if not _guard.active:
			setter.call(line.text))
	return line


func bind_option(option: OptionButton, getter: Callable, setter: Callable) -> OptionButton:
	# The option's item ids carry the model value (set_item_id when populating). sync selects the
	# item whose id matches getter(info); a user pick fires setter(selected id). Setting `selected`
	# programmatically does not emit item_selected, and the guard suppresses any echo regardless.
	_bindings.append(func(info):
		var want := int(getter.call(info))
		option.selected = -1
		for i in option.item_count:
			if option.get_item_id(i) == want:
				option.selected = i
				break)
	option.item_selected.connect(func(idx: int):
		if not _guard.active:
			setter.call(option.get_item_id(idx)))
	return option


func sync_from(info: Dictionary) -> void:
	_guard.run(func() -> void:
		for apply in _bindings:
			apply.call(info))
