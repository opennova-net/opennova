class_name InspectorDef
extends Resource

## Typed registry entry for one workflow inspector: a stable id, the UI label
## and tooltip, and the inspector script to instantiate. Replaces the former
## untyped WORKFLOW_DEFS dictionary table so a workspace declares its inspectors
## as a list of these instead of dict literals with magic keys.

@export var id: int = -1
@export var label: String = ""
@export var tooltip: String = ""
## A code-first inspector instantiated with .new() and driven via build_main().
@export var inspector_script: Script
## A scene-backed inspector instantiated and added to the host (legacy / terrain
## migration). Workspaces dispatch on whichever of the two is set.
@export var inspector_scene: PackedScene


static func make(id_value: int, label_text: String, tooltip_text: String, script_ref: Script) -> InspectorDef:
	var def := InspectorDef.new()
	def.id = id_value
	def.label = label_text
	def.tooltip = tooltip_text
	def.inspector_script = script_ref
	return def


static func make_scene(id_value: int, label_text: String, tooltip_text: String, scene_ref: PackedScene) -> InspectorDef:
	var def := InspectorDef.new()
	def.id = id_value
	def.label = label_text
	def.tooltip = tooltip_text
	def.inspector_scene = scene_ref
	return def


func to_workflow_dict() -> Dictionary:
	return {"id": id, "label": label, "tooltip": tooltip}
