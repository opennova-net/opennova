class_name NovaDebugTarget
extends RefCounted
## A re-resolving adapter at the debug-session seam.
##
## Sources are deliberately called for every read and invocation. Mission and
## world reloads replace their objects, so retaining the resolved object would
## make controls write into a stale runtime.

var id: StringName
var label: String
var source := Callable()
var unavailable_reason: String


func _init(
		target_id: StringName = &"",
		target_label: String = "",
		target_source: Callable = Callable(),
		target_unavailable_reason: String = "") -> void:
	id = target_id
	label = target_label
	source = target_source
	unavailable_reason = target_unavailable_reason


func resolve() -> Object:
	if not source.is_valid():
		return null
	var value: Variant = source.call()
	if value is Object and is_instance_valid(value):
		return value
	return null
