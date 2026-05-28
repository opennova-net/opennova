class_name WorkspaceDef
extends Resource

## Structural registry entry for one editor workspace: a stable id and the
## adapter script to instantiate, plus whether it lives in a popup (Environment)
## rather than the main workspace rail. Label, tooltip, and behavior stay
## self-described by the adapter, so this only captures what the shell needs to
## build and place the workspace.

@export var id: int = -1
@export var adapter_script: Script
@export var popup: bool = false


static func make(id_value: int, script_ref: Script, is_popup := false) -> WorkspaceDef:
	var def := WorkspaceDef.new()
	def.id = id_value
	def.adapter_script = script_ref
	def.popup = is_popup
	return def
