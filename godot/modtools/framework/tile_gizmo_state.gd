class_name TileGizmoState
extends RefCounted
## The in-world tile gizmo payload: what the shell overlay needs to place and
## caption the button card over the active workspace's selected tile. Produced
## by EditorWorkspace.get_tile_gizmo_state() overrides (null = no selection,
## gizmo hidden), consumed by TileGizmoOverlay. Typed record per ADR 0017.

var label := ""
var anchor_world := Vector3.ZERO


static func make(p_label: String, p_anchor_world: Vector3) -> TileGizmoState:
	var state := TileGizmoState.new()
	state.label = p_label
	state.anchor_world = p_anchor_world
	return state
