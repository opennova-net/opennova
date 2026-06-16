class_name WorkspaceRegistry
extends Node

## Global registry for Editor Workspaces, replacing the monolithic Enum/switch statement.

static var _workspaces: Dictionary = {}

## Registers a workspace definition by its string ID (e.g. &"terrain")
static func register_workspace(id: StringName, def: WorkspaceDef) -> void:
	_workspaces[id] = def

static func get_workspace(id: StringName) -> WorkspaceDef:
	return _workspaces.get(id)

static func get_all_workspaces() -> Array[WorkspaceDef]:
	var values: Array[WorkspaceDef] = []
	for v in _workspaces.values():
		values.append(v as WorkspaceDef)
	return values

static func clear() -> void:
	_workspaces.clear()
