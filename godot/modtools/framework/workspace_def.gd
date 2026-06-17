class_name WorkspaceDef
extends Resource

## Structural registry entry for one editor workspace: a stable id and the
## adapter script to instantiate, whether it lives in a popup (Environment)
## rather than swapping the main viewport, plus optional placement metadata the
## navigation needs before the adapter is instantiated (its category grouping and
## an icon key). Label, tooltip, and behavior stay self-described by the
## adapter, so this only captures what the shell needs to build and place the
## workspace.
##
## To add a workspace: (1) write an EditorWorkspace subclass implementing the
## hook tiers documented in framework/editor_workspace.gd (CONTRACT); (2) add a
## Workspace enum entry in editor/editor_workstation.gd; (3) append one
## WorkspaceDef.make(Workspace.X, XWorkspaceAdapter[, is_popup][, category][, icon_id])
## row to EditorWorkstation._workspace_defs(). No .tres resources, no shell
## type-switch.

@export var id: int = -1
@export var adapter_script: Script
@export var popup: bool = false
## Navigation grouping. The nav lists workspaces under a header per category, in
## the order categories first appear in the registry. &"" means no header (flat).
@export var category: StringName = &""
## Glyph key the nav rail resolves to a row icon via EditorIconLibrary.
## &"" means text-only.
@export var icon_id: StringName = &""


static func make(id_value: int, script_ref: Script, is_popup := false,
		category: StringName = &"", icon_id: StringName = &"") -> WorkspaceDef:
	var def := WorkspaceDef.new()
	def.id = id_value
	def.adapter_script = script_ref
	def.popup = is_popup
	def.category = category
	def.icon_id = icon_id
	return def
