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
var _asset_dock_host: Control
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
	set_terrain_editor(value)


func get_workspace_tooltip() -> String:
	return "Edit terrain sculpting, paint, foliage, tiles, and layout."


func shows_camera_status() -> bool:
	return true


func shows_tile_gizmo() -> bool:
	return true


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


func mount_viewport(host: Control) -> void:
	if host == null or terrain_editor == null:
		return
	var viewport := _ensure_mount().mount(host)
	if viewport != null:
		viewport.set_terrain_editor(terrain_editor)
		viewport.set_edit_input_enabled(true)


func unmount_viewport(_host: Control) -> void:
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
		_asset_dock_host = null
		return
	_asset_dock_host = dock
	if _asset_dock == null:
		_asset_dock = TerrainAssetDockScene.instantiate()
		_asset_dock.name = "TerrainAssetDock"
		_asset_dock.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_asset_dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_asset_dock_host.add_child(_asset_dock)
	if _asset_dock.has_method("set_editor"):
		_asset_dock.set_editor(terrain_editor)


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


func build_workflow_inspector(workflow_id: int, host: Control) -> void:
	var code_inspector := get_workflow_inspector(workflow_id)
	if code_inspector == null:
		return
	code_inspector.build_main(host)
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
	terrain_editor.request_new_terrain()
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


func open_file(path: String) -> Error:
	if terrain_editor == null:
		return ERR_UNAVAILABLE
	return terrain_editor.request_open_trn(path)


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
