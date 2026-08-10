class_name TerrainEditorBase
extends Node3D

## The typed surface the rest of ONED consumes off the terrain editor: the
## Terrain workspace adapter, the shared TerrainViewport, the Mission
## controller chain (which rides the terrain world for placement, picking,
## height sampling and re-grounding), the camera panel, and the MCP tools.
## TerrainEditor is the one production implementation; headless tests bind
## lightweight subclasses that override only the slice they exercise. Every
## default is the documented "no terrain loaded / capability absent" outcome
## the old has_method probes used to produce (ADR 0034: typed contract, no
## probing).

enum Tool { RAISE, LOWER, SMOOTH, FLATTEN, PAINT_DETAIL, EDIT_SECTORS, PAINT_COLORMAP, CLONE_COLOR, TILE_STAMP, FOLIAGE_PAINT, SURFACE_PAINT }

# Maps to opennova::DepthFormat in engine/formats/cpt/cpt.h.
# BHD-era terrains (DVD4, original DPTH golden) use DPTH; JO/DFX-era use CDEP.
enum ExportFlavor { BHD = 0, DFX_JO = 1 }

const INVALID_HEIGHT := -1000000.0
const INVALID_HIT := Vector3(INF, INF, INF)

# The document-shaped state rides overridable routing getters because the real
# editor computes these off its document/brush session while test subclasses
# want plain storage: override the _get_/_set_ pair, never redeclare the var.
var is_dirty: bool:
	get:
		return _get_is_dirty()
	set(value):
		_set_is_dirty(value)

var current_tool: Tool:
	get:
		return _get_current_tool()
	set(value):
		_set_current_tool(value)

var brush_radius: float:
	get:
		return _get_brush_radius()
	set(value):
		_set_brush_radius(value)

var brush_strength: float:
	get:
		return _get_brush_strength()
	set(value):
		_set_brush_strength(value)

var brush_hardness: float:
	get:
		return _get_brush_hardness()
	set(value):
		_set_brush_hardness(value)


func _get_is_dirty() -> bool:
	return false


func _set_is_dirty(_value: bool) -> void:
	pass


func _get_current_tool() -> Tool:
	return Tool.RAISE


func _set_current_tool(_value: Tool) -> void:
	pass


func _get_brush_radius() -> float:
	return 0.0


func _set_brush_radius(_value: float) -> void:
	pass


func _get_brush_strength() -> float:
	return 0.0


func _set_brush_strength(_value: float) -> void:
	pass


func _get_brush_hardness() -> float:
	return 0.0


func _set_brush_hardness(_value: float) -> void:
	pass


# --- World / sampling (the Mission chain's surface) -------------------------

func get_terrain_world_root() -> Node3D:
	return null


# FlyCamera-typed on purpose: the mission chain and the MCP camera tools frame
# through the orbit state (frame_bounds_custom), which lives on FlyCamera.
func get_editor_camera() -> FlyCamera:
	return null


func get_resource_root() -> ResourceRoot:
	return null


# The shared environment document.
func get_environment_editor() -> EnvironmentEditor:
	return null


func get_environment_node() -> MissionEnvironment:
	return null


func get_height_revision() -> int:
	return 0


func sample_height_world(_world_x: float, _world_z: float) -> float:
	return INVALID_HEIGHT


# Default rides the scalar sampler, so a test subclass may override either
# form; the real editor overrides the batch.
func sample_heights_world(points: PackedVector2Array) -> PackedFloat32Array:
	var heights := PackedFloat32Array()
	heights.resize(points.size())
	for i in points.size():
		heights[i] = sample_height_world(points[i].x, points[i].y)
	return heights


func raycast_terrain_at(_mouse_pos: Vector2) -> Vector3:
	return INVALID_HIT


func is_valid_terrain_hit(_hit: Vector3) -> bool:
	return false


func set_mission_preview_context(_tile_info: TerrainTileInfo, _preview_time_of_day: float = NAN) -> void:
	pass


func clear_mission_preview_context() -> void:
	pass


# --- Viewport / input -------------------------------------------------------

func set_viewport_active(_active: bool, _edit_input_enabled: bool = true) -> void:
	pass


func set_viewport_mouse_position(_position: Vector2) -> void:
	pass


func handle_viewport_input(_event: InputEvent) -> void:
	pass


func set_grid_guide_visible(_visible: bool) -> void:
	pass


func set_axes_visible(_visible: bool) -> void:
	pass


# --- Tools / status readouts ------------------------------------------------

func set_tool(_tool: Tool) -> void:
	pass


func get_terrain_name_value() -> String:
	return ""


func get_paint_color() -> Color:
	return Color.WHITE


func get_paint_detail_channel() -> int:
	return 0


func has_clone_source() -> bool:
	return false


func get_selected_surface_index() -> int:
	return 0


func get_selected_surface_label() -> String:
	return ""


func get_selected_foliage_def() -> TerrainFoliageDef:
	return null


func get_sector_count() -> int:
	return 0


func get_sector_rows() -> int:
	return 0


# --- Tile authoring (the shell tile gizmo's surface) ------------------------

func get_tileinfo_entry(_index: int) -> TerrainTileEntry:
	return null


func get_tileinfo_selected_index() -> int:
	return -1


func has_selected_tileinfo_entry() -> bool:
	return false


func get_selected_tileinfo_entry() -> TerrainTileEntry:
	return null


func get_selected_tileinfo_world_center() -> Vector3:
	return Vector3.ZERO


func clear_tileinfo_selection() -> void:
	pass


func get_tile_stamp_tile_index() -> int:
	return 0


func delete_selected_tileinfo_entry() -> bool:
	return false


func rotate_selected_tileinfo_clockwise() -> bool:
	return false


func flip_selected_tileinfo_x() -> bool:
	return false


func flip_selected_tileinfo_y() -> bool:
	return false


# --- Edit history -----------------------------------------------------------

func can_undo() -> bool:
	return false


func can_redo() -> bool:
	return false


func undo() -> void:
	pass


func redo() -> void:
	pass


# --- Project lifecycle / export ---------------------------------------------

func new_terrain() -> void:
	pass


func open_trn(_trn_path: String, _timeline: PerfTimeline = null) -> Error:
	return ERR_UNAVAILABLE


func save_project_to_current_dir() -> Error:
	return ERR_UNAVAILABLE


func save_project(_dir_path: String) -> Error:
	return ERR_UNAVAILABLE


func begin_export_terrain(_output_dir: String, _flavor: int = ExportFlavor.DFX_JO) -> Error:
	return ERR_UNAVAILABLE


func get_current_project_dir() -> String:
	return ""


func get_current_trn_path() -> String:
	return ""


func has_current_project_dir() -> bool:
	return false


func get_last_open_dir() -> String:
	return ""


func get_last_save_dir() -> String:
	return ""


func get_last_export_dir() -> String:
	return ""


func is_export_running() -> bool:
	return false


func get_export_progress_ratio() -> float:
	return 0.0


func get_export_progress_current() -> int:
	return 0


func get_export_progress_total() -> int:
	return 0


func get_export_progress_message() -> String:
	return ""


func get_export_progress_phase() -> String:
	return ""
