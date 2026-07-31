class_name TerrainEditorWorkspace
extends EditorWorkspace

const TerrainViewportScript = preload("res://modtools/terrain/terrain_viewport.gd")
const TerrainAssetDockScene = preload("res://modtools/terrain/ui/editor_asset_dock.tscn")

enum Workflow { SCULPT, PAINT, SCATTER, STAMP, LAYOUT }
enum ExportFlavor { BHD = 0, DFX_JO = 1 }

const DETAIL_LABELS := ["Detail A", "Detail B", "Detail C"]

# Workflow inspectors are declared as typed InspectorDef rows in
# _build_inspector_defs(); Sculpt is code-first, the rest are still scene-backed
# while the terrain port is in progress.

var terrain_editor: Node
var _asset_dock_mount: Control
var _asset_dock: Control
var _mount: ViewportMount


func _init(value: Node = null) -> void:
	terrain_editor = value


func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"TerrainViewport", func() -> Control: return TerrainViewportScript.new())
	return _mount


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value
	if _mount != null:
		var viewport := _mount.get_viewport_node()
		if viewport != null:
			viewport.set_terrain_editor(terrain_editor)


func bind_to_editor(value: Node) -> void:
	# The shell binds the app root; unwrap to the terrain domain editor. Tests
	# that bind a bare TerrainEditor keep working (no unwrap hook -> as-is).
	if value != null and value.has_method("get_terrain_editor"):
		value = value.get_terrain_editor()
	set_terrain_editor(value)


func get_workspace_tooltip() -> String:
	return "Edit terrain sculpting, paint, foliage, tiles, and layout."


func shows_camera_status() -> bool:
	return true


func shows_tile_gizmo() -> bool:
	return true


# Non-empty only while the Tile workflow has a placed-tile selection. Leaving
# TILE_STAMP clears the selection (set_tool), so gating on the active workflow
# id matches the shell's old workflow check exactly.
func get_tile_gizmo_state() -> TileGizmoState:
	if terrain_editor == null or get_active_workflow_id() != Workflow.STAMP:
		return null
	if not terrain_editor.has_selected_tileinfo_entry():
		return null
	var entry: Variant = terrain_editor.get_selected_tileinfo_entry()
	if entry == null:
		return null
	return TileGizmoState.make(
		"Editing tile %03d @ (%d, %d)" % [
			entry.get_tile_index(), entry.get_cell_x(), entry.get_cell_z()],
		terrain_editor.get_selected_tileinfo_world_center() + Vector3(0.0, 2.0, 0.0))


func run_tile_gizmo_action(action: StringName) -> void:
	if terrain_editor == null:
		return
	match action:
		&"done":
			terrain_editor.clear_tileinfo_selection()
		&"rotate":
			terrain_editor.rotate_selected_tileinfo_clockwise()
		&"flip_x":
			terrain_editor.flip_selected_tileinfo_x()
		&"flip_y":
			terrain_editor.flip_selected_tileinfo_y()
		&"delete":
			terrain_editor.delete_selected_tileinfo_entry()


# View guides: a flat y=0 reference grid would be buried under (or float
# through) sculpted heights, so the shell's grid toggle draws thin NEUTRAL
# sector-boundary lines that follow the surface (u_show_grid); axes are a
# world-origin gizmo. Deliberately NOT the Layout workflow's sector overlay —
# that is a colored per-sector diagnostic (tint fill + rainbow edges) owned by
# the Layout checkbox alone, and the two states never touch.
func shows_view_guides() -> bool:
	return true


func set_grid_visible(value: bool) -> void:
	if terrain_editor != null and terrain_editor.has_method("set_grid_guide_visible"):
		terrain_editor.set_grid_guide_visible(value)


func set_axes_visible(value: bool) -> void:
	if terrain_editor != null and terrain_editor.has_method("set_axes_visible"):
		terrain_editor.set_axes_visible(value)


func get_export_flavors() -> Array:
	return [
		{"id": ExportFlavor.BHD, "label": "BHD"},
		{"id": ExportFlavor.DFX_JO, "label": "DFX / JO"},
	]


func activate() -> void:
	if terrain_editor != null and _mount != null and _mount.is_mounted():
		terrain_editor.set_viewport_active(true, true)


func deactivate() -> void:
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)


func mount_viewport(mount: Control) -> void:
	if mount == null or terrain_editor == null:
		return
	var viewport := _ensure_mount().mount(mount)
	if viewport != null:
		viewport.set_terrain_editor(terrain_editor)
		viewport.set_edit_input_enabled(true)


func unmount_viewport(_released: Control) -> void:
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.release()


func get_viewport_camera() -> Camera3D:
	if terrain_editor != null and terrain_editor.has_method("get_editor_camera"):
		return terrain_editor.get_editor_camera()
	return null


func get_workspace_id() -> String:
	return "terrain"


func get_workspace_label() -> String:
	return "Terrain"


func get_project_title() -> String:
	if terrain_editor == null:
		return "Terrain"
	var name: String = terrain_editor.get_terrain_name_value()
	if name.is_empty():
		name = "untitled"
	var dirty := "*" if terrain_editor.is_dirty else ""
	return "%s%s" % [name, dirty]


func get_status_tool() -> String:
	if terrain_editor == null:
		return "Terrain"
	return _tool_name(terrain_editor.current_tool)


func get_status_context() -> String:
	if terrain_editor == null:
		return ""
	match terrain_editor.current_tool:
		TerrainEditor.Tool.PAINT_DETAIL:
			var channel := clampi(terrain_editor.get_paint_detail_channel(), 0, DETAIL_LABELS.size() - 1)
			return DETAIL_LABELS[channel]
		TerrainEditor.Tool.PAINT_COLORMAP:
			return "#" + terrain_editor.get_paint_color().to_html(false).to_upper()
		TerrainEditor.Tool.CLONE_COLOR:
			return "Clone source ready" if terrain_editor.has_clone_source() else "Clone source not set"
		TerrainEditor.Tool.SURFACE_PAINT:
			return "%s (%d)" % [terrain_editor.get_selected_surface_label(), terrain_editor.get_selected_surface_index()]
		TerrainEditor.Tool.FOLIAGE_PAINT:
			var def: Variant = terrain_editor.get_selected_foliage_def()
			if def == null:
				return "No foliage type selected"
			var label := String(def.graphic).strip_edges()
			if label.is_empty():
				label = "unnamed foliage type"
			return label
		TerrainEditor.Tool.TILE_STAMP:
			var selected: int = terrain_editor.get_tileinfo_selected_index()
			if selected >= 0:
				var entry: Variant = terrain_editor.get_tileinfo_entry(selected)
				if entry:
					return "Editing tile %03d at cell (%d, %d)" % [entry.get_tile_index(), entry.get_cell_x(), entry.get_cell_z()]
			return "Brush tile %03d" % terrain_editor.get_tile_stamp_tile_index()
		TerrainEditor.Tool.EDIT_SECTORS:
			return "%d x %d sector grid" % [terrain_editor.get_sector_count(), terrain_editor.get_sector_rows()]
		_:
			return "R %d  S %.2f  H %.2f" % [roundi(terrain_editor.brush_radius), terrain_editor.brush_strength, terrain_editor.brush_hardness]


func uses_asset_dock() -> bool:
	return true


func set_asset_dock(dock: Control) -> void:
	if _asset_dock != null and _asset_dock.get_parent() != null:
		_asset_dock.get_parent().remove_child(_asset_dock)
	if dock == null:
		if _asset_dock != null:
			_asset_dock.free()
			_asset_dock = null
		_asset_dock_mount = null
		return
	_asset_dock_mount = dock
	if _asset_dock == null:
		_asset_dock = TerrainAssetDockScene.instantiate()
		_asset_dock.name = "TerrainAssetDock"
		_asset_dock.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_asset_dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_asset_dock_mount.add_child(_asset_dock)
	if _asset_dock.has_method("set_editor"):
		_asset_dock.set_editor(terrain_editor)
	# "Used by" (which missions sit on this terrain) rides the shell's reference
	# index; headless owners get no strip. Injected here because the dock is the
	# one terrain surface built with the shell seam in scope (the workflow
	# inspectors receive only the terrain editor).
	var services := get_reference_services()
	if services != null and _asset_dock.has_method("set_reference_services"):
		_asset_dock.set_reference_services(services)


func sync_asset_dock() -> void:
	if _asset_dock != null and _asset_dock.has_method("sync_from_editor_state"):
		_asset_dock.sync_from_editor_state()


func _build_inspector_defs() -> Array:
	return [
		InspectorDef.make(Workflow.SCULPT, "Sculpt", "Raise, lower, smooth, and flatten the terrain.", SculptInspector),
		InspectorDef.make(Workflow.PAINT, "Paint", "Paint detail layers, color, clone, and surface types.", PaintInspector),
		InspectorDef.make(Workflow.SCATTER, "Foliage", "Manage and paint foliage placement.", ScatterInspector),
		InspectorDef.make(Workflow.STAMP, "Tile", "Place and edit tiles.", StampInspector),
		InspectorDef.make(Workflow.LAYOUT, "Layout", "Edit sectors, map size, origin, and water.", LayoutInspector),
	]


func get_active_workflow_id() -> int:
	if terrain_editor == null:
		return Workflow.SCULPT
	return _workflow_for_tool(terrain_editor.current_tool)


func activate_workflow(workflow_id: int) -> void:
	if terrain_editor == null:
		return
	if workflow_id == Workflow.LAYOUT:
		terrain_editor.set_tool(TerrainEditor.Tool.EDIT_SECTORS)


func build_workflow_inspector(workflow_id: int, mount: Control) -> void:
	var code_inspector := get_workflow_inspector(workflow_id)
	if code_inspector == null:
		return
	code_inspector.build_main(mount)
	code_inspector.set_editor(terrain_editor)


func is_busy() -> bool:
	return terrain_editor != null and terrain_editor.is_export_running()


func get_export_progress_title() -> String:
	return "Exporting terrain..."


func get_export_progress_phase() -> String:
	return terrain_editor.get_export_progress_phase() if terrain_editor != null and is_busy() else ""


func get_export_progress_message() -> String:
	return terrain_editor.get_export_progress_message() if terrain_editor != null and is_busy() else ""


func get_export_progress_current() -> int:
	return terrain_editor.get_export_progress_current() if terrain_editor != null and is_busy() else 0


func get_export_progress_total() -> int:
	return terrain_editor.get_export_progress_total() if terrain_editor != null and is_busy() else 0


func get_export_progress_ratio() -> float:
	return terrain_editor.get_export_progress_ratio() if terrain_editor != null and is_busy() else 0.0


# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return terrain_editor


func can_new() -> bool:
	return terrain_editor != null and not is_busy()


func get_new_action_label() -> String:
	return "New Terrain"


func new_current() -> Error:
	if terrain_editor == null:
		return ERR_UNAVAILABLE
	if is_busy():
		return OK
	var ed: Node = terrain_editor
	var make_new := func() -> void: ed.new_terrain()
	if _prompt_dirty_guard(make_new):
		return OK
	ed.new_terrain()
	return OK


func can_open() -> bool:
	return terrain_editor != null and not is_busy()


func get_open_action_label() -> String:
	return "Open Terrain..."


func get_open_dialog_title() -> String:
	return "Open .trn"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.trn ; Terrain"])


func get_open_dialog_dir() -> String:
	return terrain_editor.get_last_open_dir() if terrain_editor else ""


func get_open_resource_kind() -> String:
	return "terrain"


func get_current_resource_path() -> String:
	if terrain_editor != null and terrain_editor.has_method("get_current_trn_path"):
		return terrain_editor.get_current_trn_path()
	return ""


# Single entry point for "user picked a .trn". Project vs. import mode is
# auto-detected inside open_trn by the presence of a sibling <name>_depth.raw
# (project) vs. a .cpt alongside (imported game asset).
func open_file(path: String) -> Error:
	if terrain_editor == null:
		return ERR_UNAVAILABLE
	if is_busy():
		return OK
	var ed: Node = terrain_editor
	var open_it := func() -> void: ed.open_trn(path)
	if _prompt_dirty_guard(open_it):
		return OK
	return ed.open_trn(path)


# Dirty guard for actions that replace the open terrain (New/Open): pops the
# shell's shared unsaved-changes dialog and returns true when the prompt now
# owns the action. Save routes through the shell's save_then, which falls back
# to Save As while the project has no directory yet. Headless (no shell):
# nothing can prompt, so callers run the action directly.
func _prompt_dirty_guard(run: Callable) -> bool:
	if terrain_editor == null or not terrain_editor.is_dirty:
		return false
	if editor_shell == null or not editor_shell.has_method("prompt_unsaved_for"):
		return false
	# Locals only in the lambdas: capturing `self` members would hold this
	# RefCounted workspace through the shell's callable stash.
	var shell: Object = editor_shell
	var ws: EditorWorkspace = self
	shell.prompt_unsaved_for(
		func() -> void: shell.save_then(ws, run),
		run)
	return true


func can_save() -> bool:
	return terrain_editor != null and terrain_editor.is_dirty and not is_busy()


func get_save_action_label() -> String:
	return "Save Project"


func can_save_as() -> bool:
	return terrain_editor != null and not is_busy()


func get_save_as_action_label() -> String:
	return "Save Project As..."


func save_current() -> Error:
	if terrain_editor == null:
		return ERR_UNAVAILABLE
	return terrain_editor.save_project_to_current_dir()


func save_as(dir_path: String) -> Error:
	if terrain_editor == null:
		return ERR_UNAVAILABLE
	return terrain_editor.save_project(dir_path)


func can_export() -> bool:
	return terrain_editor != null and not is_busy()


func has_export_action() -> bool:
	return terrain_editor != null


func get_export_action_label() -> String:
	return "Export Terrain..."


func begin_export(dir_path: String, flavor: int) -> Error:
	if terrain_editor == null:
		return ERR_UNAVAILABLE
	return terrain_editor.begin_export_terrain(dir_path, flavor)


func get_save_dialog_title() -> String:
	return "Choose where to save your project"


func get_save_dialog_dir() -> String:
	if terrain_editor == null:
		return ""
	if terrain_editor.has_current_project_dir():
		return terrain_editor.get_current_project_dir()
	return terrain_editor.get_last_save_dir()


func get_export_dialog_title() -> String:
	return "Choose where to export"


func get_export_dialog_dir() -> String:
	if terrain_editor == null:
		return ""
	if not terrain_editor.get_last_export_dir().is_empty():
		return terrain_editor.get_last_export_dir()
	if terrain_editor.has_current_project_dir():
		return terrain_editor.get_current_project_dir()
	return terrain_editor.get_last_save_dir()


func _workflow_for_tool(tool: int) -> int:
	match tool:
		TerrainEditor.Tool.RAISE, TerrainEditor.Tool.LOWER, TerrainEditor.Tool.SMOOTH, TerrainEditor.Tool.FLATTEN:
			return Workflow.SCULPT
		TerrainEditor.Tool.PAINT_DETAIL, TerrainEditor.Tool.PAINT_COLORMAP, TerrainEditor.Tool.CLONE_COLOR, TerrainEditor.Tool.SURFACE_PAINT:
			return Workflow.PAINT
		TerrainEditor.Tool.FOLIAGE_PAINT:
			return Workflow.SCATTER
		TerrainEditor.Tool.TILE_STAMP:
			return Workflow.STAMP
		_:
			return Workflow.LAYOUT


func _tool_name(tool: int) -> String:
	match tool:
		TerrainEditor.Tool.RAISE: return "Raise"
		TerrainEditor.Tool.LOWER: return "Lower"
		TerrainEditor.Tool.SMOOTH: return "Smooth"
		TerrainEditor.Tool.FLATTEN: return "Flatten"
		TerrainEditor.Tool.PAINT_DETAIL: return "Paint detail"
		TerrainEditor.Tool.PAINT_COLORMAP: return "Paint color"
		TerrainEditor.Tool.SURFACE_PAINT: return "Paint surface"
		TerrainEditor.Tool.CLONE_COLOR: return "Clone color"
		TerrainEditor.Tool.FOLIAGE_PAINT: return "Foliage"
		TerrainEditor.Tool.TILE_STAMP: return "Tile"
		TerrainEditor.Tool.EDIT_SECTORS: return "Edit sectors"
		_:
			return ""
