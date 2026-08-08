class_name NovaDebugOptionState
extends RefCounted
## Live values for the NovaDebugOptions registry: one per overlay, shared with
## every page through NovaDebugContext. set_value() is the single write path —
## page controls, overlay.set_option and tests all route through it, so the
## bound control re-syncs (without re-firing) and `changed` fires exactly once
## per real change no matter who wrote.

signal changed(id: StringName, value: Variant)

var _values: Dictionary = {}
var _controls: Dictionary = {}


## The current value, falling back to the registry default. Unknown ids
## return null.
func value(id: StringName) -> Variant:
	if _values.has(id):
		return _values[id]
	return NovaDebugOptions.find(id).get("default")


func set_value(id: StringName, new_value: Variant) -> void:
	if NovaDebugOptions.find(id).is_empty():
		return
	if value(id) == new_value:
		return
	_values[id] = new_value
	_sync_control(id, new_value)
	changed.emit(id, new_value)


## Bind the page control rendering this option, so programmatic writes
## (set_option, tests) keep the UI in step.
func register_control(id: StringName, control: Control) -> void:
	_controls[id] = control


func _sync_control(id: StringName, new_value: Variant) -> void:
	var control: Control = _controls.get(id)
	if control == null or not is_instance_valid(control):
		return
	if control is BaseButton:
		(control as BaseButton).set_pressed_no_signal(bool(new_value))
	elif control is Slider:
		(control as Slider).set_value_no_signal(float(new_value))
	elif control is OptionButton:
		(control as OptionButton).select(int(new_value))
