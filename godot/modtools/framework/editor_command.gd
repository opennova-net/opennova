class_name EditorCommand
extends RefCounted

## Represents a declarative action exposed by a workspace or capability.
## Replaces hardcoded shell actions ("Save", "Open", etc.).

signal state_changed

var id: StringName
var label: String
var icon: Texture2D
var shortcut: Shortcut
var tooltip: String

var _execute_fn: Callable
var _can_execute_fn: Callable

func _init(p_id: StringName, p_label: String, p_execute: Callable, p_can_execute: Callable = Callable()) -> void:
	id = p_id
	label = p_label
	_execute_fn = p_execute
	_can_execute_fn = p_can_execute

func with_icon(p_icon: Texture2D) -> EditorCommand:
	icon = p_icon
	return self

func with_shortcut(p_shortcut: Shortcut) -> EditorCommand:
	shortcut = p_shortcut
	return self

func with_tooltip(p_tooltip: String) -> EditorCommand:
	tooltip = p_tooltip
	return self

func can_execute() -> bool:
	if _can_execute_fn.is_valid():
		return _can_execute_fn.call()
	return true

func execute() -> void:
	if can_execute() and _execute_fn.is_valid():
		_execute_fn.call()

func notify_state_changed() -> void:
	state_changed.emit()
