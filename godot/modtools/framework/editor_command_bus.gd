class_name EditorCommandBus
extends Node

## Global event bus for cross-workspace signaling and editor commands.
## Replaces hardcoded jump methods in the shell (e.g., open_font_workspace).

signal command_requested(command_name: StringName, payload: Dictionary)

static var _instance: EditorCommandBus

static func get_instance() -> EditorCommandBus:
	if _instance == null:
		_instance = EditorCommandBus.new()
	return _instance

static func dispatch(command_name: StringName, payload: Dictionary = {}) -> void:
	get_instance().command_requested.emit(command_name, payload)
