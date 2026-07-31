class_name FieldBinder
extends RefCounted
## Lives in engine/ui (not modtools): shell-neutral by contract, so both the
## game's debug overlay and editor inspectors can use it.
## Declarative inspector field binding. Each bind_*(control, getter, setter) pairs
## a control with a getter(info_dict)->value (read from the model snapshot) and a
## setter(value)->void (write to the model). The change signal is auto-wired and
## guarded by an internal SyncGuard, so sync_from(info) can push fresh model
## values into every control without echoing back through the setters. Replaces
## the per-field "build control + guard.active check + connect lambda + manual
## sync read-back" boilerplate. Setters route through the model (e.g.
## NovaObjectData.set_light_field) unchanged.

# Marks an OptionButton row bind_option appended itself to surface a model value that is absent
# from the caller's curated choices. Tagged so each sync can drop its own prior fallback before
# re-evaluating, without disturbing caller-populated rows.
const _OPTION_FALLBACK_META := "__field_binder_fallback__"

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
	# `shown` tracks the text we last DISPLAYED (via a sync write or a commit), so focus-out can tell a
	# genuine user edit (text changed since) from an unchanged field whose model moved underneath it.
	var state := { "shown": line.text }
	_bindings.append(func(info):
		var v: String = getter.call(info)
		if line.text != v and not line.has_focus():
			line.text = v
			state.shown = v)
	# Commit on BOTH Enter (text_submitted) and focus-out. Focus-out is essential: leaving a field for
	# another one WITHOUT pressing Enter must persist the edit. But commit ONLY when the text actually
	# changed since it was last displayed: a model `changed` that arrived while this field was focused
	# was skipped by the sync above (still showing the old text), so an unconditional focus-out commit
	# would write that stale text back over the newer model value (e.g. reverting an undo). The guard
	# suppresses echoes during a programmatic sync_from.
	line.text_submitted.connect(func(value: String):
		if not _guard.active:
			setter.call(value)
			state.shown = value)
	line.focus_exited.connect(func():
		if not _guard.active and line.text != state.shown:
			setter.call(line.text)
			state.shown = line.text)
	return line


func bind_option(option: OptionButton, getter: Callable, setter: Callable, options_getter := Callable(), fallback_label := Callable()) -> OptionButton:
	# The option's item ids carry the model value (set_item_id when populating). sync selects the
	# item whose id matches getter(info); a user pick fires setter(selected id). Setting `selected`
	# programmatically does not emit item_selected, and the guard suppresses any echo regardless.
	#
	# When options_getter is supplied, this binder also OWNS the item list: each sync refills it from
	# options_getter() (an Array of { id, label }, cached upstream so this is cheap). That keeps populate
	# + select + out-of-range fallback in ONE place -- callers must NOT also populate the list (a second
	# populator would fight this one and leave a duplicate/untagged fallback row).
	_bindings.append(func(info):
		var want := int(getter.call(info))
		if options_getter.is_valid():
			option.clear()  # also drops any prior fallback row
			for opt in options_getter.call():
				var oi := option.item_count
				option.add_item(String((opt as Dictionary).get("label", "")))
				option.set_item_id(oi, int((opt as Dictionary).get("id", 0)))
		else:
			# The caller populated the list; drop any fallback row a previous sync appended (the model
			# value may now be in range, or moved to a different out-of-range value). Walk back-to-front.
			for i in range(option.item_count - 1, -1, -1):
				if option.get_item_metadata(i) == _OPTION_FALLBACK_META:
					option.remove_item(i)
		option.selected = -1
		for i in option.item_count:
			if option.get_item_id(i) == want:
				option.selected = i
				break
		if option.selected == -1:
			# The model holds a value outside the offered choices (e.g. a shipped enum the editor does
			# not enumerate, or a valid-but-empty waypoint slot). Surface it as a raw row so the control
			# shows the real value instead of rendering blank. fallback_label lets a caller name the value
			# in domain terms ("Path 125 (no markers)") instead of the generic "Value 125"; mirrors
			# InspectorForms.populate_id_option's fallback otherwise.
			var idx := option.item_count
			var fb_text := String(fallback_label.call(want)) if fallback_label.is_valid() else "Value %d" % want
			option.add_item(fb_text)
			option.set_item_id(idx, want)
			option.set_item_metadata(idx, _OPTION_FALLBACK_META)
			option.selected = idx)
	option.item_selected.connect(func(idx: int):
		if not _guard.active:
			setter.call(option.get_item_id(idx)))
	return option


func bind_link(widget: Object, getter: Callable, setter: Callable) -> Object:
	# Duck-typed link-widget contract, so this binder stays shell-neutral while the
	# widget itself (e.g. the editor's ResourceRefWidget) lives with its owner:
	# the widget exposes set_value(String) / get_value() and a value_changed(String)
	# signal, set_value MUST NOT emit value_changed, and the widget owns its own
	# skip-while-typing behavior (mirroring bind_line's focused-skip).
	_bindings.append(func(info):
		var v: String = getter.call(info)
		if widget.get_value() != v:
			widget.set_value(v))
	widget.value_changed.connect(func(value: String):
		if not _guard.active:
			setter.call(value))
	return widget


func sync_from(info: Dictionary) -> void:
	_guard.run(func() -> void:
		for apply in _bindings:
			apply.call(info))


# Force the reentrancy guard clear. GDScript cannot try/finally, so if a bound getter/setter errors
# mid-sync the guard would stay stuck true and silence every field write; callers clear it at the top
# of their refresh so a stuck guard self-heals within one cycle rather than disabling the panel.
func reset_guard() -> void:
	_guard.active = false
