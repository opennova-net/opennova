class_name InspectorDef
extends Resource

## Typed registry entry for one workflow inspector: a stable id, the UI label
## and tooltip, and the inspector script to instantiate. Replaces the former
## untyped WORKFLOW_DEFS dictionary table so a workspace declares its inspectors
## as a list of these instead of dict literals with magic keys.
##
## To add a workflow inspector: (1) write an inspector script — extend
## WorkflowInspector / ListDetailInspector (object domain) or TerrainInspector
## (terrain domain); (2) add one InspectorDef.make(id, label, tooltip, Script)
## row to the workspace's _build_inspector_defs(). The shell renders the rail
## button and instantiates the script on demand.

@export var id: int = -1
@export var label: String = ""
@export var tooltip: String = ""
## The inspector Script. The shell instantiates it on demand and drives it via
## build_main(mount): terrain workflows use .new(), object workflows use
## .new(self) so the inspector receives its owning workspace.
@export var inspector_script: Script


static func make(id_value: int, label_text: String, tooltip_text: String, script_ref: Script) -> InspectorDef:
	var def := InspectorDef.new()
	def.id = id_value
	def.label = label_text
	def.tooltip = tooltip_text
	def.inspector_script = script_ref
	return def
