class_name TerrainEditor
extends Node3D

signal ui_state_changed(version: int)

enum Tool { RAISE, LOWER, SMOOTH, FLATTEN, PAINT_DETAIL, EDIT_SECTORS, PAINT_COLORMAP, CLONE_COLOR, TILE_STAMP, FOLIAGE_PAINT, SURFACE_PAINT }
enum TileInteractionMode { PLACE, EDIT_SELECTED }

# Maps to opennova::DepthFormat in libs/cpt/include/cpt/cpt.h.
# BHD-era terrains (DVD4, original DPTH golden) use DPTH; JO/DFX-era use CDEP.
enum ExportFlavor { BHD = 0, DFX_JO = 1 }

const HM_SIZE := 1024
const DEFAULT_HEIGHT := 20.0
const INVALID_HEIGHT := -1000000.0
const INVALID_HIT := Vector3(INF, INF, INF)
const EDITOR_MIN_WINDOW_SIZE := Vector2i(1366, 768)
const TerrainEditorBrushes = preload("res://modtools/terrain/terrain_editor_brushes.gd")
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditorSurfacePaint = preload("res://modtools/terrain/terrain_editor_surface_paint.gd")
const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const TerrainEditorDocument = preload("res://modtools/terrain/terrain_editor_document.gd")
const TerrainEditorBrushSession = preload("res://modtools/terrain/terrain_editor_brush_session.gd")
const CDEPConstraint = preload("res://modtools/terrain/terrain_editor_cdep_constraint.gd")
const TerrainFoliagePreview = preload("res://modtools/terrain/terrain_foliage_preview.gd")
const TerrainTileOverlayPreview = preload("res://modtools/terrain/terrain_tile_overlay_preview.gd")
const DEFAULT_SECTOR_PATTERN := [
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 1, 3, 0, 0, 0,
	0, 0, 0, 2, 4, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
]

@onready var terrain_mesh: EditorTerrainMesh = $EditorTerrainMesh
@onready var camera: Camera3D = $FlyCamera
@onready var workstation: TerrainEditorWorkstation = $CanvasLayer/EditorWorkstation

var _document: TerrainEditorDocument = TerrainEditorDocument.new()
var _brush_session: TerrainEditorBrushSession = TerrainEditorBrushSession.new()

var _data: NovaTerrainData:
	get:
		return _document.data
	set(value):
		_document.data = value

var current_tool: Tool:
	get:
		return _brush_session.current_tool
	set(value):
		_brush_session.current_tool = value

var brush_radius: float:
	get:
		return _brush_session.brush_radius
	set(value):
		_brush_session.brush_radius = value

var brush_strength: float:
	get:
		return _brush_session.brush_strength
	set(value):
		_brush_session.brush_strength = value

var brush_hardness: float:
	get:
		return _brush_session.brush_hardness
	set(value):
		_brush_session.brush_hardness = value

var brush_active: bool:
	get:
		return _brush_session.brush_active
	set(value):
		_brush_session.brush_active = value

var flatten_target_height: float:
	get:
		return _brush_session.flatten_target_height
	set(value):
		_brush_session.flatten_target_height = value

var flatten_target_set: bool:
	get:
		return _brush_session.flatten_target_set
	set(value):
		_brush_session.flatten_target_set = value

var paint_detail_channel: int:
	get:
		return _brush_session.paint_detail_channel
	set(value):
		_brush_session.paint_detail_channel = value

var paint_color: Color:
	get:
		return _brush_session.paint_color
	set(value):
		_brush_session.paint_color = value

var selected_surface_index: int = TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX

var texture_files: Dictionary:
	get:
		return _document.texture_files
	set(value):
		_document.texture_files = value

var _heightmap_image: Image:
	get:
		return _document.heightmap_image
	set(value):
		_document.heightmap_image = value

var _colormap_image: Image:
	get:
		return _document.colormap_image
	set(value):
		_document.colormap_image = value

var _colormap_tex: ImageTexture:
	get:
		return _document.colormap_tex
	set(value):
		_document.colormap_tex = value

var _blendmap_image: Image:
	get:
		return _document.blendmap_image
	set(value):
		_document.blendmap_image = value

var _blendmap_tex: ImageTexture:
	get:
		return _document.blendmap_tex
	set(value):
		_document.blendmap_tex = value

var _hover_hit := Vector3(-1.0, -1.0, -1.0)
var _hover_hit_valid: bool = false
var _active_sector_cell := Vector2i(-1, -1)

var _stroke_last_hit: Vector3:
	get:
		return _brush_session._stroke_last_hit
	set(value):
		_brush_session._stroke_last_hit = value

var _stroke_has_last_hit: bool:
	get:
		return _brush_session._stroke_has_last_hit
	set(value):
		_brush_session._stroke_has_last_hit = value

var _export_job: NovaTerrainBuildJob
var _export_output_dir: String = ""

var _history:
	get:
		return _brush_session._history

var _stroke_kind: int:
	get:
		return _brush_session._stroke_kind
	set(value):
		_brush_session._stroke_kind = value

var _stroke_invert: bool:
	get:
		return _brush_session._stroke_invert
	set(value):
		_brush_session._stroke_invert = value

var _paint_texture_image: Image:
	get:
		return _brush_session._paint_texture_image
	set(value):
		_brush_session._paint_texture_image = value

var _paint_texture_filename: String:
	get:
		return _brush_session._paint_texture_filename
	set(value):
		_brush_session._paint_texture_filename = value

var _clone_source_set: bool:
	get:
		return _brush_session._clone_source_set
	set(value):
		_brush_session._clone_source_set = value

var _clone_source_world: Vector3:
	get:
		return _brush_session._clone_source_world
	set(value):
		_brush_session._clone_source_world = value

var _clone_source_image: Image:
	get:
		return _brush_session._clone_source_image
	set(value):
		_brush_session._clone_source_image = value

var _clone_offset_px: Vector2i:
	get:
		return _brush_session._clone_offset_px
	set(value):
		_brush_session._clone_offset_px = value

var _clone_source_marker: MeshInstance3D

# Diagnostic: trace height values through load/edit/export to compare with NovaTerrain.
const _HEIGHT_DEBUG := false

var _water_instance: MeshInstance3D
var _water_plane_mesh: PlaneMesh
var _water_material: StandardMaterial3D
var water_visible: bool = true
var sector_overlay_visible: bool = false

var _foliage_preview: TerrainFoliagePreview
var _tile_overlay_preview: TerrainTileOverlayPreview
var _tile_interaction_mode: int = TileInteractionMode.PLACE
var _surface_map_stroke_before: Dictionary = {}
var _surface_map_stroke_changed: bool = false
var _foliage_map_stroke_before: Dictionary = {}
var _foliage_map_stroke_changed: bool = false

var is_dirty: bool:
	get:
		return _document.is_dirty
	set(value):
		_document.is_dirty = value
		_mark_ui_state_changed()

var _last_open_dir: String = ""
var _last_save_dir: String = ""
var _last_export_dir: String = ""
var _pending_unsaved_action: Callable = Callable()
var _pending_unsaved_action_name: String = ""
var _previous_window_min_size: Vector2i = Vector2i.ZERO
var _ui_state_version: int = 0


func _ready() -> void:
	get_tree().auto_accept_quit = false
	_configure_editor_window()
	camera.position = Vector3(512, 80, 600)
	camera.rotation_degrees = Vector3(-30, 0, 0)
	_init_water_plane()
	_init_foliage_preview()
	_init_tile_overlay_preview()
	_init_clone_marker()
	if camera.has_signal("quit_requested"):
		camera.connect("quit_requested", Callable(self, "request_quit_editor"))
	_load_editor_state()
	if workstation and workstation.has_method("set_editor"):
		workstation.set_editor(self)
	new_terrain()


func _exit_tree() -> void:
	var window := get_window()
	if window:
		window.min_size = _previous_window_min_size


func _configure_editor_window() -> void:
	var window := get_window()
	if window == null:
		return
	_previous_window_min_size = window.min_size
	window.min_size = EDITOR_MIN_WINDOW_SIZE
	if window.mode == Window.MODE_WINDOWED:
		var next_size := window.size
		next_size.x = maxi(next_size.x, EDITOR_MIN_WINDOW_SIZE.x)
		next_size.y = maxi(next_size.y, EDITOR_MIN_WINDOW_SIZE.y)
		if next_size != window.size:
			window.size = next_size


## Forward a short status message to the workstation UI.
func _notify_status(message: String) -> void:
	if workstation and workstation.has_method("show_status_message"):
		workstation.show_status_message(message)


func _init_clone_marker() -> void:
	var sphere := SphereMesh.new()
	sphere.radius = 0.8
	sphere.height = 1.6
	sphere.radial_segments = 12
	sphere.rings = 6

	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color(1.0, 0.85, 0.2, 1.0)
	mat.no_depth_test = true

	_clone_source_marker = MeshInstance3D.new()
	_clone_source_marker.name = "CloneSourceMarker"
	_clone_source_marker.mesh = sphere
	_clone_source_marker.material_override = mat
	_clone_source_marker.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_clone_source_marker.visible = false
	add_child(_clone_source_marker)


func _init_tile_overlay_preview() -> void:
	_tile_overlay_preview = TerrainTileOverlayPreview.new()
	_tile_overlay_preview.name = "TileOverlayPreview"
	add_child(_tile_overlay_preview)


func _init_foliage_preview() -> void:
	_foliage_preview = TerrainFoliagePreview.new()
	_foliage_preview.name = "FoliagePreview"
	add_child(_foliage_preview)


func _init_water_plane() -> void:
	_water_plane_mesh = PlaneMesh.new()
	_water_plane_mesh.size = Vector2(1024.0, 1024.0)

	_water_material = StandardMaterial3D.new()
	_water_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_water_material.albedo_color = Color(0.13, 0.34, 0.55, 0.55)
	_water_material.metallic = 0.1
	_water_material.roughness = 0.2
	_water_material.cull_mode = BaseMaterial3D.CULL_DISABLED

	_water_instance = MeshInstance3D.new()
	_water_instance.name = "WaterPlane"
	_water_instance.mesh = _water_plane_mesh
	_water_instance.material_override = _water_material
	_water_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_water_instance)


func _update_water_plane() -> void:
	if _water_instance == null:
		return
	var has_bounds := false
	var center_x := 0.0
	var center_z := 0.0
	if terrain_mesh:
		var bounds: AABB = terrain_mesh.get_world_bounds()
		has_bounds = bounds.size.x > 0.0 and bounds.size.z > 0.0
		if has_bounds:
			var margin := 0.2
			_water_plane_mesh.size = Vector2(
				bounds.size.x * (1.0 + margin),
				bounds.size.z * (1.0 + margin)
			)
			center_x = bounds.position.x + bounds.size.x * 0.5
			center_z = bounds.position.z + bounds.size.z * 0.5
	_water_instance.position = Vector3(center_x, float(get_water_height()), center_z)
	_water_instance.visible = water_visible and has_bounds


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed:
		var focus_owner := get_viewport().gui_get_focus_owner()
		if focus_owner != null:
			focus_owner.release_focus()

	if is_export_running():
		if event is InputEventMouseButton:
			var locked_mouse := event as InputEventMouseButton
			if locked_mouse.button_index == MOUSE_BUTTON_LEFT and not locked_mouse.pressed:
				_on_primary_end()
		return

	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			if mouse_button.pressed:
				_on_primary_start()
			else:
				_on_primary_end()
		elif mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed:
			if current_tool == Tool.TILE_STAMP and is_tile_edit_mode():
				clear_tileinfo_selection()

	if event is InputEventKey and event.is_pressed() and not event.is_echo():
		var key := event as InputEventKey
		if key.keycode == KEY_ESCAPE and current_tool == Tool.TILE_STAMP and _document.get_tileinfo_selected_index() >= 0:
			clear_tileinfo_selection()
			get_viewport().set_input_as_handled()
			return
		if key.keycode == KEY_DELETE and _document.get_tileinfo_selected_index() >= 0:
			delete_selected_tileinfo_entry()
			return
		if current_tool == Tool.TILE_STAMP and _document.get_tileinfo_selected_index() >= 0 and not key.ctrl_pressed and not key.alt_pressed:
			match key.keycode:
				KEY_R:
					if rotate_selected_tileinfo_clockwise():
						get_viewport().set_input_as_handled()
					return
				KEY_X:
					if flip_selected_tileinfo_x():
						get_viewport().set_input_as_handled()
					return
				KEY_Y:
					if flip_selected_tileinfo_y():
						get_viewport().set_input_as_handled()
					return
		if key.ctrl_pressed:
			if key.keycode == KEY_Z:
				if key.shift_pressed:
					redo()
				else:
					undo()
				return
			if key.keycode == KEY_Y:
				redo()
				return
		match key.keycode:
			KEY_1: set_tool(Tool.RAISE)
			KEY_2: set_tool(Tool.LOWER)
			KEY_3: set_tool(Tool.SMOOTH)
			KEY_4: set_tool(Tool.FLATTEN)
			KEY_5: select_detail_paint_channel(0)
			KEY_6: select_detail_paint_channel(1)
			KEY_7: select_detail_paint_channel(2)
			KEY_8: set_tool(Tool.EDIT_SECTORS)
			KEY_9: set_tool(Tool.PAINT_COLORMAP)
			KEY_C: set_tool(Tool.CLONE_COLOR)
			KEY_T: set_tool(Tool.TILE_STAMP)
			KEY_F: set_tool(Tool.FOLIAGE_PAINT)
			KEY_G: set_tool(Tool.SURFACE_PAINT)
			KEY_BRACKETLEFT:
				set_brush_radius_value(maxf(1.0, brush_radius - 4.0))
			KEY_BRACKETRIGHT:
				set_brush_radius_value(minf(128.0, brush_radius + 4.0))
			KEY_SEMICOLON:
				set_brush_strength_value(maxf(0.01, brush_strength - 0.05))
			KEY_APOSTROPHE:
				set_brush_strength_value(minf(5.0, brush_strength + 0.05))
			KEY_COMMA:
				set_brush_hardness_value(maxf(0.0, brush_hardness - 0.05))
			KEY_PERIOD:
				set_brush_hardness_value(minf(1.0, brush_hardness + 0.05))


func _process(delta: float) -> void:
	_poll_export_job()

	_hover_hit = _raycast_terrain()
	_hover_hit_valid = _is_valid_hit(_hover_hit)

	var material := _get_material()
	if _is_brush_preview_tool() and _hover_hit_valid:
		material.set_shader_parameter("u_brush_pos", Vector2(_hover_hit.x, _hover_hit.z))
		material.set_shader_parameter("u_brush_radius", brush_radius)
		material.set_shader_parameter("u_brush_hardness", brush_hardness)
	else:
		material.set_shader_parameter("u_brush_pos", Vector2(-10000.0, -10000.0))
	_sync_surface_overlay_state(material)

	if brush_active and _is_brush_preview_tool():
		_apply_brush_stroke(delta)

	_sync_foliage_preview()
	_sync_tile_overlay_preview()


func set_tool(tool: Tool) -> void:
	if is_export_running():
		return
	if current_tool == Tool.TILE_STAMP and tool != Tool.TILE_STAMP and has_selected_tileinfo_entry():
		_document.clear_tileinfo_selection()
		_mark_tile_overlay_dirty()
	if tool != Tool.TILE_STAMP:
		_tile_interaction_mode = TileInteractionMode.PLACE
	current_tool = tool
	flatten_target_set = false
	_sync_surface_overlay_state(_get_material())
	_update_hud()


func _sync_surface_overlay_state(material: ShaderMaterial) -> void:
	if material == null:
		return
	var overlay_enabled := current_tool == Tool.SURFACE_PAINT
	var preview_color := Color(1.0, 1.0, 0.2)
	if overlay_enabled:
		var preview_index := _get_surface_paint_index(Input.is_key_pressed(KEY_CTRL))
		preview_color = TerrainEditorSurfacePaint.get_surface_color(preview_index, _document.get_surface_palette_bytes())
	material.set_shader_parameter("u_show_surface_overlay", overlay_enabled)
	material.set_shader_parameter("u_show_sector_overlay", sector_overlay_visible)
	material.set_shader_parameter("u_brush_color", preview_color)


func _get_surface_paint_index(invert: bool) -> int:
	if invert:
		return TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX
	return selected_surface_index


func select_detail_paint_channel(channel: int) -> void:
	if is_export_running():
		return
	paint_detail_channel = channel
	set_tool(Tool.PAINT_DETAIL)


func set_paint_color(color: Color) -> void:
	if paint_color == color:
		return
	paint_color = color
	_update_hud()


func get_paint_color() -> Color:
	return paint_color


func get_terrain_name_value() -> String:
	return _document.get_terrain_name()


func set_terrain_name_value(value: String) -> void:
	if is_export_running() or not _data:
		return
	var next_value := value.strip_edges()
	if next_value.is_empty():
		next_value = "untitled"
	_data.set_terrain_name(next_value)
	is_dirty = true
	_update_hud()


func get_detail_density() -> int:
	return _data.get_detail_density() if _data else 0


func set_detail_density_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	_data.set_detail_density(value)
	_document.sync_material_from_data(_get_material())
	is_dirty = true
	_update_hud()


func get_detail_density2() -> int:
	return _data.get_detail_density2() if _data else 0


func set_detail_density2_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	_data.set_detail_density2(value)
	is_dirty = true
	_update_hud()


func get_wrap_x_enabled() -> bool:
	return _data.get_wrap_x() if _data else false


func set_wrap_x_enabled(enabled: bool) -> void:
	if is_export_running() or not _data:
		return
	_data.set_wrap_x(enabled)
	is_dirty = true
	_update_hud()


func get_wrap_y_enabled() -> bool:
	return _data.get_wrap_y() if _data else false


func set_wrap_y_enabled(enabled: bool) -> void:
	if is_export_running() or not _data:
		return
	_data.set_wrap_y(enabled)
	is_dirty = true
	_update_hud()

func get_data() -> NovaTerrainData:
	return _data


func get_metadata_summary() -> Dictionary:
	var foliage_summary: Array = []
	for def in _document.foliage_defs:
		if def != null:
			foliage_summary.append(def.to_dictionary())
	return {
		"charmap": _document.get_slot_filename("charmap"),
		"foliagemap": _document.get_slot_filename("foliagemap"),
		"tilestrip": _document.get_slot_filename("tilestrip"),
		"tileinfo": _document.tileinfo_filename,
		"foliage_defs": foliage_summary,
	}


func get_surface_types() -> Array:
	return TerrainEditorSurfacePaint.get_surface_types()


func get_selected_surface_index() -> int:
	return selected_surface_index


func set_selected_surface_index(value: int) -> void:
	selected_surface_index = clampi(value, 0, 255)
	_update_hud()


func get_surface_palette_bytes() -> PackedByteArray:
	return _document.get_surface_palette_bytes()


func get_selected_surface_label() -> String:
	return TerrainEditorSurfacePaint.get_surface_label(selected_surface_index)


func get_selected_surface_color() -> Color:
	return TerrainEditorSurfacePaint.get_surface_color(selected_surface_index, get_surface_palette_bytes())


const FOLIAGE_DEFS_LIMIT := 4


func add_foliage_def() -> void:
	if is_export_running():
		return
	if _document.foliage_defs.size() >= FOLIAGE_DEFS_LIMIT:
		return
	var before_state := _document.capture_foliage_editor_history_state()
	if not _document.add_foliage_def():
		return
	var after_state := _document.capture_foliage_editor_history_state()
	_push_foliage_defs_history(before_state, after_state)
	is_dirty = true
	_mark_foliage_preview_dirty()
	_sync_hud_from_editor()


func remove_foliage_def(index: int) -> void:
	if is_export_running():
		return
	if index < 0 or index >= _document.foliage_defs.size():
		return
	var before_state := _document.capture_foliage_editor_history_state()
	if not _document.remove_foliage_def(index):
		return
	var after_state := _document.capture_foliage_editor_history_state()
	_push_foliage_defs_history(before_state, after_state)
	is_dirty = true
	_mark_foliage_preview_dirty()
	_sync_hud_from_editor()


func set_foliage_def_field(index: int, field: String, value: Variant) -> void:
	if is_export_running():
		return
	if index < 0 or index >= _document.foliage_defs.size():
		return
	var def := _document.foliage_defs[index]
	if def == null:
		return
	var before_state := _document.capture_foliage_editor_history_state()
	match field:
		"graphic":
			if def.graphic == String(value):
				return
			def.graphic = String(value)
		"color_lower":
			if def.color_lower == int(value):
				return
			def.color_lower = int(value)
		"color_upper":
			if def.color_upper == int(value):
				return
			def.color_upper = int(value)
		"shadow":
			if def.shadow == bool(value):
				return
			def.shadow = bool(value)
		"force_on":
			if def.force_on == bool(value):
				return
			def.force_on = bool(value)
		"attrib_flags":
			if def.attrib_flags == int(value):
				return
			def.attrib_flags = int(value)
		_:
			return
	var after_state := _document.capture_foliage_editor_history_state()
	_push_foliage_defs_history(before_state, after_state)
	is_dirty = true
	_mark_foliage_preview_dirty()
	_sync_hud_from_editor()


func save_project_to_current_dir() -> Error:
	if _document.current_project_dir.is_empty():
		return ERR_INVALID_PARAMETER
	return save_project(_document.current_project_dir)


func get_current_tool() -> Tool:
	return current_tool


func get_paint_detail_channel() -> int:
	return paint_detail_channel


func set_brush_radius_value(value: float) -> void:
	if is_export_running():
		return
	brush_radius = clampf(value, 1.0, 128.0)
	_update_hud()


func set_brush_strength_value(value: float) -> void:
	if is_export_running():
		return
	brush_strength = clampf(value, 0.01, 5.0)
	_update_hud()


func set_brush_hardness_value(value: float) -> void:
	if is_export_running():
		return
	brush_hardness = clampf(value, 0.0, 1.0)
	_update_hud()


func set_shader_flag(param: String, value: bool) -> void:
	_get_material().set_shader_parameter(param, value)


func get_sector_count() -> int:
	return _data.get_sector_count() if _data else 0


func get_sector_size() -> int:
	return maxi(get_sector_count(), get_sector_rows())


func set_sector_count(value: int) -> void:
	set_sector_size(value)


func get_sector_rows() -> int:
	return _data.get_sector_rows() if _data else 0


func set_sector_rows(value: int) -> void:
	set_sector_size(value)


func set_sector_size(value: int) -> void:
	if is_export_running() or not _data:
		return
	var next := clampi(value, 1, 16)
	if _data.get_sector_count() == next and _data.get_sector_rows() == next:
		return
	_data.set_sector_count(next)
	_data.set_sector_rows(next)
	_sync_sector_layout(true)
	is_dirty = true


func _normalize_sector_layout_if_needed() -> bool:
	if _data == null:
		return false
	var cols := clampi(_data.get_sector_count(), 1, 16)
	var rows := clampi(_data.get_sector_rows(), 1, 16)
	var size := maxi(cols, rows)
	if cols == size and rows == size:
		return false
	_data.set_sector_count(size)
	_data.set_sector_rows(size)
	_notify_status("Normalized sector grid to %d x %d. Non-square layouts are no longer supported." % [size, size])
	return true


func get_origin_x() -> int:
	return _data.get_origin_x() if _data else 0


func set_origin_x_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	_data.set_origin_x(value)
	_sync_sector_layout(true)
	is_dirty = true


func get_origin_y() -> int:
	return _data.get_origin_y() if _data else 0


func set_origin_y_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	_data.set_origin_y(value)
	_sync_sector_layout(true)
	is_dirty = true


func get_water_height() -> int:
	return int(_data.get_water_height() / 2) if _data else 0


func set_water_height_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	_data.set_water_height(value * 2)
	_update_water_plane()
	is_dirty = true


func is_water_visible() -> bool:
	return water_visible


func set_water_visible(visible: bool) -> void:
	if water_visible == visible:
		return
	water_visible = visible
	_update_water_plane()
	_update_hud()


func is_sector_overlay_visible() -> bool:
	return sector_overlay_visible


func set_sector_overlay_visible(visible: bool) -> void:
	sector_overlay_visible = visible
	if terrain_mesh:
		_sync_surface_overlay_state(_get_material())
	_update_hud()


func is_export_running() -> bool:
	return _export_job != null


func get_export_progress_ratio() -> float:
	if _export_job == null:
		return 0.0
	return _export_job.get_progress_ratio()


func get_export_progress_current() -> int:
	if _export_job == null:
		return 0
	return _export_job.get_progress_current()


func get_export_progress_total() -> int:
	if _export_job == null:
		return 0
	return _export_job.get_progress_total()


func get_export_progress_message() -> String:
	if _export_job == null:
		return ""
	return _export_job.get_progress_message()


func get_export_progress_phase() -> String:
	if _export_job == null:
		return ""
	return _export_job.get_progress_phase()


func get_editor_camera() -> Camera3D:
	return camera


func get_sector_cell(row: int, col: int) -> int:
	if not _data:
		return 0
	var idx := row * 16 + col
	var grid: PackedInt32Array = _data.get_sector_grid()
	if idx < 0 or idx >= grid.size():
		return 0
	return clampi(grid[idx], 0, 4)


func set_sector_cell(row: int, col: int, value: int) -> bool:
	if is_export_running() or not _data:
		return false
	var idx := row * 16 + col
	var grid: PackedInt32Array = _data.get_sector_grid()
	if idx < 0 or idx >= grid.size():
		return false
	var next := clampi(value, 0, 4)
	_active_sector_cell = Vector2i(row, col)
	if grid[idx] == next:
		_update_hud()
		return false
	grid[idx] = next
	_data.set_sector_grid(grid)
	_sync_sector_layout(false)
	is_dirty = true
	return true


func get_active_sector_cell() -> Vector2i:
	return _active_sector_cell


func get_slot_texture(slot_id: String) -> Texture2D:
	if slot_id == "foliagemap":
		return _document.get_foliage_map_preview_texture()
	return TerrainEditorSlots.get_slot_texture(_data, slot_id)


func get_slot_filename(slot_id: String) -> String:
	return String(texture_files.get(slot_id, ""))


func get_tileinfo_summary() -> Dictionary:
	return _document.get_tileinfo_summary()


func get_foliage_defs() -> Array[NovaTerrainFoliageDef]:
	return _document.foliage_defs


func get_foliage_map() -> NovaTerrainFoliageMap:
	return _document.foliage_map


func get_foliage_preview_summary() -> Dictionary:
	if _foliage_preview == null:
		return {}
	_sync_foliage_preview()
	return _foliage_preview.get_preview_summary()


func get_selected_foliage_def_index() -> int:
	return _document.selected_foliage_def_index


func set_selected_foliage_def_index(index: int) -> void:
	_document.set_selected_foliage_def_index(index)
	_mark_foliage_preview_dirty()
	_sync_hud_from_editor()


func get_selected_foliage_def() -> NovaTerrainFoliageDef:
	return _document.get_selected_foliage_def()


func has_tileinfo_resource() -> bool:
	return _document.has_tileinfo_resource()


func get_tileinfo_entries() -> Array:
	return _document.get_tileinfo_entries()


func get_tileinfo_entry(index: int) -> NovaTerrainTileEntry:
	return _document.get_tileinfo_entry(index)


func get_tileinfo_selected_index() -> int:
	return _document.get_tileinfo_selected_index()


func get_tileinfo_entry_indices_at_cell(cell_x: int, cell_z: int) -> PackedInt32Array:
	return _document.get_tileinfo_entry_indices_at_cell(cell_x, cell_z)


func has_selected_tileinfo_entry() -> bool:
	return get_selected_tileinfo_entry() != null


func get_selected_tileinfo_entry() -> NovaTerrainTileEntry:
	return _document.get_tileinfo_entry(_document.get_tileinfo_selected_index())


func get_selected_tileinfo_world_center() -> Vector3:
	var entry := get_selected_tileinfo_entry()
	if entry == null:
		return Vector3.ZERO
	return TerrainTileOverlayPreview.entry_center_world(entry, terrain_mesh)


func is_tile_edit_mode() -> bool:
	return current_tool == Tool.TILE_STAMP and _tile_interaction_mode == TileInteractionMode.EDIT_SELECTED and has_selected_tileinfo_entry()


func is_tile_placement_mode() -> bool:
	return current_tool == Tool.TILE_STAMP and not is_tile_edit_mode()


func clear_tileinfo_selection() -> void:
	_document.clear_tileinfo_selection()
	_tile_interaction_mode = TileInteractionMode.PLACE
	_mark_tile_overlay_dirty()
	_sync_hud_from_editor()


func select_tileinfo_entry(index: int, focus_camera: bool = false) -> void:
	_document.set_tileinfo_selected_index(index, false)
	_tile_interaction_mode = TileInteractionMode.EDIT_SELECTED
	_mark_tile_overlay_dirty()
	_sync_hud_from_editor()
	if focus_camera:
		focus_selected_tileinfo_entry()


func get_tile_stamp_tile_index() -> int:
	return _document.get_tile_stamp_tile_index()


func set_tile_stamp_tile_index(value: int) -> void:
	if is_export_running():
		return
	_document.set_tile_stamp_tile_index(value)
	_sync_hud_from_editor()


func get_tile_stamp_flags() -> int:
	return _document.get_tile_stamp_flags()


func set_tile_stamp_flags(value: int) -> void:
	if is_export_running():
		return
	_document.set_tile_stamp_flags(value)
	_sync_hud_from_editor()


func apply_tile_stamp_to_selected_entry() -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _document.apply_stamp_to_selected_tileinfo_entry())


func delete_selected_tileinfo_entry() -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	var changed := _finalize_tileinfo_edit(before_state, _document.delete_selected_tileinfo_entry())
	if changed:
		_tile_interaction_mode = TileInteractionMode.PLACE
	return changed


func replace_selected_tileinfo_tile_index(value: int) -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _document.set_selected_tileinfo_tile_index(value))


func set_selected_tileinfo_flags(value: int) -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _document.set_selected_tileinfo_flags(value))


func rotate_selected_tileinfo_clockwise() -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _document.rotate_selected_tileinfo_clockwise())


func flip_selected_tileinfo_x() -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _document.flip_selected_tileinfo_x())


func flip_selected_tileinfo_y() -> bool:
	if is_export_running():
		return false
	var before_state := _document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _document.flip_selected_tileinfo_y())


func focus_selected_tileinfo_entry() -> bool:
	var entry := _document.get_tileinfo_entry(_document.get_tileinfo_selected_index())
	if entry == null:
		return false
	var center := TerrainTileOverlayPreview.entry_center_world(entry, terrain_mesh)
	center.y += 4.0
	if camera and camera.has_method("frame_bounds"):
		camera.frame_bounds(center, maxf(float(NovaTerrainTileInfo.CELL_WORLD_SIZE) * 8.0, 96.0))
		return true
	return false


func load_texture_slot(slot_id: String, path: String) -> void:
	if is_export_running() or not _data:
		return
	if _document.load_texture_slot(_get_material(), slot_id, path):
		is_dirty = true
		if slot_id == "tilestrip":
			_mark_tile_overlay_dirty()
		elif slot_id == "foliagemap":
			_mark_foliage_preview_dirty()
		_sync_hud_from_editor()


func reset_texture_slot(slot_id: String) -> void:
	if is_export_running() or not _data:
		return
	_document.reset_texture_slot(_get_material(), slot_id)
	is_dirty = true
	if slot_id == "tilestrip":
		_mark_tile_overlay_dirty()
	elif slot_id == "foliagemap":
		_mark_foliage_preview_dirty()
	_sync_hud_from_editor()


func load_tileinfo(path: String) -> void:
	if is_export_running():
		return
	if _document.load_tileinfo(path):
		_normalize_loaded_tileinfo_if_needed()
		is_dirty = true
		_mark_tile_overlay_dirty()
		_sync_hud_from_editor()


func new_tileinfo() -> void:
	if is_export_running():
		return
	_document.new_tileinfo()
	is_dirty = true
	_mark_tile_overlay_dirty()
	_sync_hud_from_editor()


func reset_tileinfo() -> void:
	if is_export_running():
		return
	_document.reset_tileinfo()
	is_dirty = _document.is_dirty
	_mark_tile_overlay_dirty()
	_sync_hud_from_editor()


func load_paint_texture(path: String) -> void:
	if is_export_running():
		return
	if not _brush_session.load_paint_texture(path):
		return
	_update_hud()


func clear_paint_texture() -> void:
	_brush_session.clear_paint_texture()
	_update_hud()


func get_paint_texture() -> Image:
	return _brush_session.get_paint_texture()


func get_paint_texture_filename() -> String:
	return _brush_session.get_paint_texture_filename()


func has_paint_texture() -> bool:
	return _brush_session.has_paint_texture()


func _tile_cell_from_world(world_x: float, world_z: float) -> Vector2i:
	return Vector2i(
		int(floor(world_x / float(NovaTerrainTileInfo.CELL_WORLD_SIZE))),
		int(floor(world_z / float(NovaTerrainTileInfo.CELL_WORLD_SIZE)))
	)


func _stamp_tileinfo_at_hover() -> bool:
	if not _hover_hit_valid:
		return false
	if not _document.has_tileinfo_resource():
		_notify_status("Load or create a tile layout before placing tiles.")
		return false

	var cell := _tile_cell_from_world(_hover_hit.x, _hover_hit.z)
	var before_state := _document.capture_tileinfo_history_state()
	var result := _document.stamp_tileinfo_cell(cell.x, cell.y)
	if int(result.get("index", -1)) >= 0:
		_tile_interaction_mode = TileInteractionMode.EDIT_SELECTED
	return _finalize_tileinfo_edit(before_state, bool(result.get("changed", false)))


func _select_tileinfo_entry_at_hover() -> bool:
	if not _hover_hit_valid or not _document.has_tileinfo_resource():
		return false

	var cell := _tile_cell_from_world(_hover_hit.x, _hover_hit.z)
	var index := _document.find_tileinfo_entry_index_at_cell(cell.x, cell.y)
	if index < 0:
		clear_tileinfo_selection()
		return false

	select_tileinfo_entry(index, false)
	return true


func _finalize_tileinfo_edit(before_state: Dictionary, changed: bool) -> bool:
	if not changed:
		return false
	var after_state := _document.capture_tileinfo_history_state()
	_push_tileinfo_history(before_state, after_state)
	is_dirty = _document.is_dirty
	_mark_tile_overlay_dirty()
	_sync_hud_from_editor()
	return true


func _normalize_loaded_tileinfo_if_needed() -> bool:
	var removed_count := _document.normalize_tileinfo_for_editor()
	if removed_count <= 0:
		return false
	var plural := "y" if removed_count == 1 else "ies"
	_notify_status("Flattened %d stacked tile entr%s. Tile mode now supports one tile per cell." % [removed_count, plural])
	_mark_tile_overlay_dirty()
	return true


func _push_tileinfo_history(before_state: Dictionary, after_state: Dictionary) -> void:
	_brush_session.push_custom_snapshot(TerrainEditHistory.Kind.TILEINFO, before_state, after_state)


func _push_surface_map_history(before_state: Dictionary, after_state: Dictionary) -> void:
	_brush_session.push_custom_snapshot(TerrainEditHistory.Kind.SURFACEMAP, before_state, after_state)


func _push_foliage_map_history(before_state: Dictionary, after_state: Dictionary) -> void:
	_brush_session.push_custom_snapshot(TerrainEditHistory.Kind.FOLIAGEMAP, before_state, after_state)


func _push_foliage_defs_history(before_state: Variant, after_state: Variant) -> void:
	_brush_session.push_custom_snapshot(TerrainEditHistory.Kind.FOLIAGE_DEFS, before_state, after_state)


func _mark_foliage_preview_dirty() -> void:
	if _foliage_preview:
		_foliage_preview.mark_dirty()


func _sync_foliage_preview() -> void:
	if _foliage_preview == null:
		return
	_foliage_preview.set_preview_state(
		terrain_mesh,
		camera,
		_document.foliage_map,
		_document.foliage_defs,
		_document.selected_foliage_def_index
	)
	_foliage_preview.rebuild_if_needed()


func _apply_foliage_paint_stroke(delta: float) -> bool:
	if _document.foliage_map == null or not _hover_hit_valid:
		_stroke_has_last_hit = false
		return false

	var target_index := 0
	if not Input.is_key_pressed(KEY_CTRL):
		target_index = _document.get_selected_foliage_paint_index()
		if target_index < 0:
			_stroke_has_last_hit = false
			return false

	var start_hit: Vector3 = _stroke_last_hit if _stroke_has_last_hit else _hover_hit
	var end_hit := _hover_hit
	var spacing := maxf(1.0, brush_radius * 0.25)
	var distance := Vector2(end_hit.x - start_hit.x, end_hit.z - start_hit.z).length()
	var dab_count: int = 1 if not _stroke_has_last_hit else maxi(1, int(ceil(distance / spacing)))
	var map_width := maxi(_document.foliage_map.get_width(), 1)
	var map_height := maxi(_document.foliage_map.get_height(), 1)
	var radius_pixels := maxi(1, int(round(brush_radius * float(map_width) / float(HM_SIZE))))
	var changed := false

	for dab_idx in range(dab_count):
		var t: float = 1.0 if dab_count == 1 else float(dab_idx + 1) / float(dab_count)
		var dab_hit := start_hit.lerp(end_hit, t)
		var source := terrain_mesh.world_to_source_coords(dab_hit.x, dab_hit.z)
		if source.x < 0.0 or source.y < 0.0:
			continue
		var center_x := _document.foliage_map.map_x_from_heightmap_x(source.x)
		var center_y := _document.foliage_map.map_y_from_heightmap_y(source.y)
		if center_x < 0 or center_y < 0 or center_x >= map_width or center_y >= map_height:
			continue
		changed = _document.foliage_map.paint_circle(
			center_x,
			center_y,
			radius_pixels,
			brush_hardness,
			brush_strength,
			target_index
		) or changed

	_stroke_last_hit = end_hit
	_stroke_has_last_hit = true
	if changed:
		_foliage_map_stroke_changed = true
	return changed


func _eyedrop_foliage_at_hover() -> bool:
	if _document.foliage_map == null or not _hover_hit_valid:
		return false
	var source := terrain_mesh.world_to_source_coords(_hover_hit.x, _hover_hit.z)
	if source.x < 0.0 or source.y < 0.0:
		return false
	var map_x := _document.foliage_map.map_x_from_heightmap_x(source.x)
	var map_y := _document.foliage_map.map_y_from_heightmap_y(source.y)
	var match_index := int(_document.foliage_map.get_index(map_x, map_y))
	var def_index := _document.find_foliage_def_index_by_match(match_index)
	if def_index < 0:
		return false
	_document.set_selected_foliage_def_index(def_index)
	_mark_foliage_preview_dirty()
	_sync_hud_from_editor()
	return true


func _mark_tile_overlay_dirty() -> void:
	if _tile_overlay_preview:
		_tile_overlay_preview.mark_dirty()


func _tileinfo_entry_index_at_hover() -> int:
	if not _hover_hit_valid or not _document.has_tileinfo_resource():
		return -1
	var cell := _tile_cell_from_world(_hover_hit.x, _hover_hit.z)
	return _document.find_tileinfo_entry_index_at_cell(cell.x, cell.y)


func _tileinfo_ghost_state() -> Dictionary:
	return {
		"enabled": false,
		"cell": Vector2i.ZERO,
		"tile_index": _document.get_tile_stamp_tile_index(),
		"flags": _document.get_tile_stamp_flags(),
	}


func _sync_tile_overlay_preview() -> void:
	if _tile_overlay_preview == null:
		return
	var tilestrip: Texture2D = null
	if _data:
		tilestrip = TerrainEditorSlots.get_slot_texture(_data, "tilestrip")
	var ghost_state := _tileinfo_ghost_state()
	var hover_index := -1
	if current_tool == Tool.TILE_STAMP and _hover_hit_valid and _document.has_tileinfo_resource():
		var ghost_cell := _tile_cell_from_world(_hover_hit.x, _hover_hit.z)
		hover_index = _document.find_tileinfo_entry_index_at_cell(ghost_cell.x, ghost_cell.y)
		if hover_index < 0:
			ghost_state = {
				"enabled": true,
				"cell": ghost_cell,
				"tile_index": _document.get_tile_stamp_tile_index(),
				"flags": _document.get_tile_stamp_flags(),
			}
	_tile_overlay_preview.set_preview_state(
		terrain_mesh,
		_document.tileinfo_resource,
		tilestrip,
		_document.get_tileinfo_selected_index(),
		hover_index,
		bool(ghost_state.get("enabled", false)),
		ghost_state.get("cell", Vector2i.ZERO),
		int(ghost_state.get("tile_index", 0)),
		int(ghost_state.get("flags", 0))
	)
	_tile_overlay_preview.rebuild_if_needed()


func _apply_surface_paint_stroke(_delta: float) -> bool:
	if _document.surface_map_state.is_empty() or not _hover_hit_valid:
		_stroke_has_last_hit = false
		return false

	var map_width := int(_document.surface_map_state.get("width", 0))
	var map_height := int(_document.surface_map_state.get("height", 0))
	if map_width <= 0 or map_height <= 0:
		_stroke_has_last_hit = false
		return false

	var start_hit: Vector3 = _stroke_last_hit if _stroke_has_last_hit else _hover_hit
	var end_hit := _hover_hit
	var spacing := maxf(1.0, brush_radius * 0.25)
	var distance := Vector2(end_hit.x - start_hit.x, end_hit.z - start_hit.z).length()
	var dab_count: int = 1 if not _stroke_has_last_hit else maxi(1, int(ceil(distance / spacing)))
	var radius_pixels := maxi(1, int(round(brush_radius * float(map_width) / float(HM_SIZE))))
	var target_index := _get_surface_paint_index(Input.is_key_pressed(KEY_CTRL))
	var changed := false

	for dab_idx in range(dab_count):
		var t: float = 1.0 if dab_count == 1 else float(dab_idx + 1) / float(dab_count)
		var dab_hit := start_hit.lerp(end_hit, t)
		var source := terrain_mesh.world_to_source_coords(dab_hit.x, dab_hit.z)
		if source.x < 0.0 or source.y < 0.0:
			continue
		var center_x := TerrainEditorSurfacePaint.map_x_from_heightmap_x(source.x, map_width)
		var center_y := TerrainEditorSurfacePaint.map_y_from_heightmap_y(source.y, map_height)
		if center_x < 0 or center_y < 0 or center_x >= map_width or center_y >= map_height:
			continue
		changed = TerrainEditorSurfacePaint.paint_circle(
			_document.surface_map_state,
			center_x,
			center_y,
			radius_pixels,
			brush_hardness,
			brush_strength,
			target_index
		) or changed

	if changed:
		_document.restore_surface_map_history_state(_get_material(), _document.surface_map_state)
		_surface_map_stroke_changed = true

	_stroke_last_hit = end_hit
	_stroke_has_last_hit = true
	return changed


func _eyedrop_surface_at_hover() -> bool:
	if _document.surface_map_state.is_empty() or not _hover_hit_valid:
		return false
	var map_width := int(_document.surface_map_state.get("width", 0))
	var map_height := int(_document.surface_map_state.get("height", 0))
	if map_width <= 0 or map_height <= 0:
		return false
	var source := terrain_mesh.world_to_source_coords(_hover_hit.x, _hover_hit.z)
	if source.x < 0.0 or source.y < 0.0:
		return false
	var map_x := TerrainEditorSurfacePaint.map_x_from_heightmap_x(source.x, map_width)
	var map_y := TerrainEditorSurfacePaint.map_y_from_heightmap_y(source.y, map_height)
	selected_surface_index = TerrainEditorSurfacePaint.get_index(_document.surface_map_state, map_x, map_y)
	_update_hud()
	return true


func _set_clone_source(world_pos: Vector3) -> void:
	_brush_session.set_clone_source(world_pos, _colormap_image)
	if _clone_source_marker:
		_clone_source_marker.position = world_pos
		_clone_source_marker.visible = true
	_update_hud()


func clear_clone_source() -> void:
	_brush_session.clear_clone_source()
	if _clone_source_marker:
		_clone_source_marker.visible = false
	_update_hud()


func has_clone_source() -> bool:
	return _brush_session.has_clone_source()


func _on_primary_start() -> void:
	if is_export_running():
		return
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		return
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE):
		return
	if current_tool == Tool.EDIT_SECTORS:
		return
	if current_tool == Tool.TILE_STAMP:
		var hover_index := _tileinfo_entry_index_at_hover()
		if is_tile_edit_mode():
			if hover_index < 0:
				clear_tileinfo_selection()
				get_viewport().set_input_as_handled()
			elif hover_index != _document.get_tileinfo_selected_index():
				_select_tileinfo_entry_at_hover()
				get_viewport().set_input_as_handled()
			else:
				get_viewport().set_input_as_handled()
			return
		if hover_index < 0:
			_stamp_tileinfo_at_hover()
		else:
			_select_tileinfo_entry_at_hover()
		return
	if current_tool == Tool.SURFACE_PAINT:
		if not _hover_hit_valid or _document.surface_map_state.is_empty():
			return
		if Input.is_key_pressed(KEY_ALT):
			_eyedrop_surface_at_hover()
			return
		_surface_map_stroke_before = _document.capture_surface_map_history_state()
		_surface_map_stroke_changed = false
		brush_active = true
		_stroke_has_last_hit = false
		_apply_surface_paint_stroke(1.0 / 60.0)
		return
	if current_tool == Tool.FOLIAGE_PAINT:
		if not _hover_hit_valid or _document.foliage_map == null:
			return
		if Input.is_key_pressed(KEY_ALT):
			_eyedrop_foliage_at_hover()
			return
		if not Input.is_key_pressed(KEY_CTRL):
			var selected_def := _document.get_selected_foliage_def()
			if selected_def == null or selected_def.get_match() < 0:
				return
		_foliage_map_stroke_before = _document.capture_foliage_map_history_state()
		_foliage_map_stroke_changed = false
		brush_active = true
		_stroke_has_last_hit = false
		_apply_foliage_paint_stroke(1.0 / 60.0)
		return
	if current_tool == Tool.CLONE_COLOR:
		if Input.is_key_pressed(KEY_CTRL) and _hover_hit_valid:
			_set_clone_source(_hover_hit)
			_update_hud()
			return
		if not _clone_source_set or not _hover_hit_valid:
			return
		if not _brush_session.prepare_clone_drag(_hover_hit, terrain_mesh):
			return
	# fall through to normal brush-active path
	if current_tool == Tool.PAINT_COLORMAP and Input.is_key_pressed(KEY_ALT) and _hover_hit_valid:
		var source := terrain_mesh.world_to_source_coords(_hover_hit.x, _hover_hit.z)
		if source.x >= 0.0:
			paint_color = TerrainEditorBrushes.sample_colormap(_colormap_image, source.x, source.y)
			_update_hud()
		return
	_brush_session.begin_brush_drag(_source_image_for_kind(_brush_session.history_kind_for_tool(current_tool)), Input.is_key_pressed(KEY_CTRL))


func _on_primary_end() -> void:
	if current_tool == Tool.SURFACE_PAINT:
		brush_active = false
		_stroke_has_last_hit = false
		if _surface_map_stroke_changed:
			var after_state := _document.capture_surface_map_history_state()
			_push_surface_map_history(_surface_map_stroke_before, after_state)
			is_dirty = true
			_sync_hud_from_editor()
		_surface_map_stroke_before = {}
		_surface_map_stroke_changed = false
		return
	if current_tool == Tool.FOLIAGE_PAINT:
		brush_active = false
		_stroke_has_last_hit = false
		if _foliage_map_stroke_changed:
			var after_state := _document.capture_foliage_map_history_state()
			_push_foliage_map_history(_foliage_map_stroke_before, after_state)
			is_dirty = true
			_mark_foliage_preview_dirty()
			_sync_hud_from_editor()
		_foliage_map_stroke_before = {}
		_foliage_map_stroke_changed = false
		return
	var result := _brush_session.end_brush_drag(_source_image_for_kind(_stroke_kind))
	if result.get("history_committed", false):
		if _HEIGHT_DEBUG and result.get("history_kind", -1) == TerrainEditHistory.Kind.HEIGHTMAP:
			_log_stroke_height_delta()


func _log_stroke_height_delta() -> void:
	if _history._undo_stack.is_empty():
		return
	var snap: Dictionary = _history._undo_stack.back()
	var rect: Rect2i = snap["rect"]
	var before: Image = snap["before"]
	var after: Image = snap["after"]
	if rect.size.x <= 0 or rect.size.y <= 0:
		return
	var lx := rect.size.x / 2
	var ly := rect.size.y / 2
	var bx := rect.position.x + lx
	var by := rect.position.y + ly
	var before_v: float = before.get_pixel(lx, ly).r
	var after_v: float = after.get_pixel(lx, ly).r
	print("[height-debug] STROKE at rect center atlas (%d,%d): before=%.4f after=%.4f delta=%.4f" % [bx, by, before_v, after_v, after_v - before_v])


func _raycast_terrain() -> Vector3:
	var mouse_pos := get_viewport().get_mouse_position()
	var from := camera.project_ray_origin(mouse_pos)
	var direction := camera.project_ray_normal(mouse_pos)
	var bounds_hit := _intersect_ray_xz_bounds(from, direction)
	if bounds_hit.y < bounds_hit.x:
		return INVALID_HIT

	var t_entry := bounds_hit.x
	var t_exit := bounds_hit.y
	var entry_pos := from + direction * t_entry
	var entry_height := terrain_mesh.sample_world_height(entry_pos.x, entry_pos.z)
	if entry_height == INVALID_HEIGHT:
		entry_height = 0.0

	var planar_distance := Vector2(
		(from + direction * t_exit).x - entry_pos.x,
		(from + direction * t_exit).z - entry_pos.z
	).length()
	var steps := clampi(int(ceil(planar_distance / 8.0)), 24, 512)

	var prev_t := t_entry
	var prev_diff := entry_pos.y - entry_height
	var prev_valid := terrain_mesh.sample_world_height(entry_pos.x, entry_pos.z) != INVALID_HEIGHT
	if prev_valid and prev_diff <= 0.0:
		return Vector3(entry_pos.x, entry_height, entry_pos.z)

	for step in range(1, steps + 1):
		var t := lerpf(t_entry, t_exit, float(step) / float(steps))
		var pos := from + direction * t
		var height := terrain_mesh.sample_world_height(pos.x, pos.z)
		var valid := height != INVALID_HEIGHT
		if valid:
			var diff := pos.y - height
			if prev_valid and prev_diff > 0.0 and diff <= 0.0:
				return _refine_ray_hit(from, direction, prev_t, t)
			prev_t = t
			prev_diff = diff
			prev_valid = true
		else:
			prev_valid = false

	return INVALID_HIT


func _intersect_ray_xz_bounds(from: Vector3, direction: Vector3) -> Vector2:
	var bounds := terrain_mesh.get_world_bounds()
	if bounds.size.x <= 0.0 or bounds.size.z <= 0.0:
		return Vector2(1.0, -1.0)

	var min_t := -INF
	var max_t := INF
	var min_x := bounds.position.x
	var max_x := bounds.position.x + bounds.size.x
	var min_z := bounds.position.z
	var max_z := bounds.position.z + bounds.size.z

	if absf(direction.x) < 0.00001:
		if from.x < min_x or from.x > max_x:
			return Vector2(1.0, -1.0)
	else:
		var tx1 := (min_x - from.x) / direction.x
		var tx2 := (max_x - from.x) / direction.x
		min_t = maxf(min_t, minf(tx1, tx2))
		max_t = minf(max_t, maxf(tx1, tx2))

	if absf(direction.z) < 0.00001:
		if from.z < min_z or from.z > max_z:
			return Vector2(1.0, -1.0)
	else:
		var tz1 := (min_z - from.z) / direction.z
		var tz2 := (max_z - from.z) / direction.z
		min_t = maxf(min_t, minf(tz1, tz2))
		max_t = minf(max_t, maxf(tz1, tz2))

	if max_t < maxf(min_t, 0.0):
		return Vector2(1.0, -1.0)
	return Vector2(maxf(min_t, 0.0), max_t)


func _refine_ray_hit(from: Vector3, direction: Vector3, t_min: float, t_max: float) -> Vector3:
	var lo := t_min
	var hi := t_max
	for _iteration in 8:
		var mid := (lo + hi) * 0.5
		var pos := from + direction * mid
		var height := terrain_mesh.sample_world_height(pos.x, pos.z)
		if height == INVALID_HEIGHT:
			lo = mid
			continue
		if pos.y > height:
			lo = mid
		else:
			hi = mid

	var hit_pos := from + direction * hi
	var hit_height := terrain_mesh.sample_world_height(hit_pos.x, hit_pos.z)
	if hit_height == INVALID_HEIGHT:
		return INVALID_HIT
	return Vector3(hit_pos.x, hit_height, hit_pos.z)


func _is_valid_hit(hit: Vector3) -> bool:
	return is_finite(hit.x) and is_finite(hit.y) and is_finite(hit.z)


func _apply_brush_stroke(delta: float) -> void:
	if current_tool == Tool.SURFACE_PAINT:
		if _apply_surface_paint_stroke(delta):
			is_dirty = true
		return
	if current_tool == Tool.FOLIAGE_PAINT:
		if _apply_foliage_paint_stroke(delta):
			is_dirty = true
			_mark_foliage_preview_dirty()
		return
	var result := _brush_session.apply_brush_stroke(delta, _hover_hit, _hover_hit_valid, terrain_mesh, _heightmap_image, _blendmap_image, _colormap_image)
	if result["changed_heightmap"]:
		terrain_mesh.set_heightmap(_heightmap_image)
		_mark_tile_overlay_dirty()
	if result["changed_blendmap"]:
		_blendmap_tex.update(_blendmap_image)
	if result["changed_colormap"]:
		_colormap_tex.update(_colormap_image)
	if result["changed_heightmap"] or result["changed_blendmap"] or result["changed_colormap"]:
		is_dirty = true


func undo() -> void:
	if is_export_running() or not _brush_session.can_undo():
		return
	var snapshot := _brush_session.pop_undo()
	_apply_history_snapshot(snapshot, true)


func redo() -> void:
	if is_export_running() or not _brush_session.can_redo():
		return
	var snapshot := _brush_session.pop_redo()
	_apply_history_snapshot(snapshot, false)


func can_undo() -> bool:
	return _brush_session.can_undo()


func can_redo() -> bool:
	return _brush_session.can_redo()


func _apply_history_snapshot(snapshot: Dictionary, is_undo: bool) -> void:
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.SURFACEMAP:
		var surface_state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_document.restore_surface_map_history_state(_get_material(), surface_state)
		is_dirty = true
		_sync_hud_from_editor()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.TILEINFO:
		var state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_document.restore_tileinfo_history_state(state)
		is_dirty = true
		_mark_tile_overlay_dirty()
		_sync_hud_from_editor()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.FOLIAGEMAP:
		var state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_document.restore_foliage_map_history_state(state)
		is_dirty = true
		_mark_foliage_preview_dirty()
		_sync_hud_from_editor()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.FOLIAGE_DEFS:
		var defs_state: Variant = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		if defs_state is Dictionary:
			_document.restore_foliage_editor_history_state(defs_state)
		else:
			_document.restore_foliage_defs_history_state(defs_state)
		is_dirty = true
		_mark_foliage_preview_dirty()
		_sync_hud_from_editor()
		return
	var result := _brush_session.apply_history_snapshot(snapshot, is_undo, _heightmap_image, _blendmap_image, _colormap_image)
	if result["changed_heightmap"]:
		terrain_mesh.set_heightmap(_heightmap_image)
		_mark_tile_overlay_dirty()
	if result["changed_blendmap"]:
		_blendmap_tex.update(_blendmap_image)
	if result["changed_colormap"]:
		_colormap_tex.update(_colormap_image)
	if result["changed_heightmap"] or result["changed_blendmap"] or result["changed_colormap"]:
		is_dirty = true


func _history_kind_for_tool(tool: Tool) -> int:
	return _brush_session.history_kind_for_tool(tool)


func _source_image_for_kind(kind: int) -> Image:
	match kind:
		TerrainEditHistory.Kind.HEIGHTMAP:
			return _heightmap_image
		TerrainEditHistory.Kind.BLENDMAP:
			return _blendmap_image
		TerrainEditHistory.Kind.COLORMAP:
			return _colormap_image
	return null


func _update_hud() -> void:
	_mark_ui_state_changed()


func _sync_hud_from_editor() -> void:
	_mark_ui_state_changed()


func get_ui_state_version() -> int:
	return _ui_state_version


func _mark_ui_state_changed() -> void:
	_ui_state_version += 1
	ui_state_changed.emit(_ui_state_version)
	if workstation and workstation.has_method("sync_from_editor_state"):
		workstation.sync_from_editor_state()


func is_document_dirty() -> bool:
	return is_dirty


func has_pending_unsaved_action() -> bool:
	return _pending_unsaved_action.is_valid()


func get_current_project_dir() -> String:
	return _document.current_project_dir


func has_current_project_dir() -> bool:
	return not _document.current_project_dir.is_empty()


func get_last_open_dir() -> String:
	return _last_open_dir


func get_last_save_dir() -> String:
	return _last_save_dir


func get_last_export_dir() -> String:
	return _last_export_dir


func request_new_terrain() -> void:
	if _queue_unsaved_action("create a new terrain", Callable(self, "new_terrain")):
		return
	new_terrain()


func request_open_trn(trn_path: String) -> Error:
	# Single entry point for "user picked a .trn". Project vs. import mode is
	# auto-detected inside open_trn by the presence of a sibling <name>_depth.raw
	# (project) vs. a .cpt alongside (imported game asset).
	if _queue_unsaved_action("open a terrain", Callable(self, "open_trn").bind(trn_path)):
		return OK
	return open_trn(trn_path)


func request_quit_editor() -> void:
	if _queue_unsaved_action("quit", Callable(self, "_quit_editor")):
		return
	_quit_editor()


func confirm_pending_action_save() -> void:
	if not _pending_unsaved_action.is_valid():
		return
	if _document.current_project_dir.is_empty():
		if workstation and workstation.has_method("prompt_save_directory_for_pending_action"):
			workstation.prompt_save_directory_for_pending_action(_pending_unsaved_action_name)
		return
	var err := save_project(_document.current_project_dir)
	if err == OK:
		_execute_pending_action()
	else:
		_notify_status("Save failed (error %d)" % err)


func confirm_pending_action_save_as(dir_path: String) -> void:
	if not _pending_unsaved_action.is_valid():
		return
	var err := save_project(dir_path)
	if err == OK:
		_execute_pending_action()
	else:
		_notify_status("Save failed (error %d)" % err)


func confirm_pending_action_discard() -> void:
	if not _pending_unsaved_action.is_valid():
		return
	_execute_pending_action()


func cancel_pending_action() -> void:
	_clear_pending_action()


func _queue_unsaved_action(action_name: String, action: Callable) -> bool:
	if is_export_running():
		return true
	if not is_dirty:
		return false
	_pending_unsaved_action = action
	_pending_unsaved_action_name = action_name
	if workstation and workstation.has_method("prompt_unsaved_changes"):
		workstation.prompt_unsaved_changes(action_name)
	return true


func _execute_pending_action() -> void:
	var action := _pending_unsaved_action
	_clear_pending_action()
	if action.is_valid():
		action.call()


func _clear_pending_action() -> void:
	_pending_unsaved_action = Callable()
	_pending_unsaved_action_name = ""


func _quit_editor() -> void:
	get_tree().quit()


func _load_editor_state() -> void:
	var config := ConfigFile.new()
	if config.load("user://terrain_editor_state.cfg") != OK:
		return
	_last_open_dir = String(config.get_value("paths", "last_open_dir", ""))
	_last_save_dir = String(config.get_value("paths", "last_save_dir", ""))
	_last_export_dir = String(config.get_value("paths", "last_export_dir", ""))


func _save_editor_state() -> void:
	var config := ConfigFile.new()
	config.set_value("paths", "last_open_dir", _last_open_dir)
	config.set_value("paths", "last_save_dir", _last_save_dir)
	config.set_value("paths", "last_export_dir", _last_export_dir)
	config.save("user://terrain_editor_state.cfg")


func _remember_open_path(path: String) -> void:
	_last_open_dir = path.get_base_dir()
	_save_editor_state()


func _remember_save_dir(dir_path: String) -> void:
	_last_save_dir = dir_path
	_save_editor_state()


func _remember_export_dir(dir_path: String) -> void:
	_last_export_dir = dir_path
	_save_editor_state()


func new_terrain() -> void:
	if is_export_running():
		return
	_brush_session.clear_history()
	clear_clone_source()
	_create_default_document("untitled")
	_set_heightmap_image(_create_heightmap_image(DEFAULT_HEIGHT))
	_sync_sector_layout(true)
	_mark_foliage_preview_dirty()
	is_dirty = false
	_sync_hud_from_editor()


func open_trn(trn_path: String) -> Error:
	if is_export_running():
		return ERR_BUSY
	_brush_session.clear_history()
	clear_clone_source()
	_data = NovaTerrainData.new()
	_data.set_trn_path(trn_path)
	var err := _data.load()
	if err != OK:
		return err

	texture_files = {}
	_apply_default_visual_state(false)

	# Prefer a sibling <name>_depth.raw if present (project mode — depth.raw
	# is the authoring source of truth). Fall back to building the heightmap
	# from the CPT (import mode — original game assets have a CPT but no
	# depth.raw). With CPT optional, a project .trn may have neither; in
	# that case _build_heightmap_from_data returns a zero image.
	var dir_path := trn_path.get_base_dir()
	var depth_path := dir_path + "/" + _data.get_terrain_name() + "_depth.raw"
	var depth_bytes: PackedByteArray
	if FileAccess.file_exists(depth_path):
		var depth_file := FileAccess.open(depth_path, FileAccess.READ)
		if depth_file:
			depth_bytes = depth_file.get_buffer(HM_SIZE * HM_SIZE * 2)
			depth_file.close()
	if depth_bytes.size() == HM_SIZE * HM_SIZE * 2:
		_set_heightmap_image(_build_heightmap_from_raw16(depth_bytes))
	else:
		_set_heightmap_image(_build_heightmap_from_data())

	_apply_loaded_textures_from_data()
	var normalized := _normalize_sector_layout_if_needed()
	_sync_sector_layout(true)
	_document.capture_trn_resource(_data)
	_document.load_tileinfo_from_dir(dir_path)
	var normalized_tileinfo := _normalize_loaded_tileinfo_if_needed()
	_document.current_trn_path = trn_path
	# When opening a .trn that sits next to its depth.raw + texture assets,
	# treat the directory as the current project dir (Save uses it as the
	# default target). If none of those sidecars exist this is an import of
	# an external .trn; leave project_dir empty so the next Save prompts.
	if depth_bytes.size() > 0:
		_document.current_project_dir = dir_path
	else:
		_document.current_project_dir = ""
	_remember_open_path(trn_path)

	is_dirty = normalized or normalized_tileinfo
	_mark_foliage_preview_dirty()
	_update_hud()
	_sync_hud_from_editor()
	_check_loaded_cdep_violations()
	return OK


func save_project(dir_path: String) -> Error:
	if is_export_running():
		return ERR_BUSY
	DirAccess.make_dir_recursive_absolute(dir_path)
	_document.normalize_foliage_state_for_editor()
	# The terrain name is the project directory's basename. Renaming the
	# terrain is done by Save-As'ing into a differently-named directory —
	# the name field in the properties panel is read-only on purpose, so
	# there is no way for the terrain name and the dir name to drift apart.
	var name := dir_path.get_file()
	if name.is_empty():
		name = "untitled"
	if _data and String(_data.get_terrain_name()) != name:
		_data.set_terrain_name(name)

	var raw16 := _image_to_raw16(_heightmap_image)
	var depth_path := dir_path + "/" + name + "_depth.raw"
	var depth_file: FileAccess = FileAccess.open(depth_path, FileAccess.WRITE)
	if not depth_file:
		return ERR_FILE_CANT_WRITE
	depth_file.store_buffer(raw16)
	depth_file.close()

	var err := _save_texture_assets(dir_path, name)
	if err != OK:
		return err
	err = _document.save_tileinfo(dir_path, name)
	if err != OK:
		return err

	# Project save writes the .trn with no polydata — CPT is an export-time
	# bake artifact, not an authoring one. NovaTerrainData::load() tolerates
	# missing CPT since the "make CPT optional" change.
	_document.prepare_data_for_trn_save(name, "")
	err = ResourceSaver.save(_data, dir_path + "/" + name + ".trn")
	if err != OK:
		return err

	_document.current_project_dir = dir_path
	_document.current_trn_path = dir_path + "/" + name + ".trn"
	_remember_save_dir(dir_path)
	is_dirty = false
	_update_hud()
	_sync_hud_from_editor()
	return OK


# Heightmaps loaded from project .trn files can predate the editor's live
# CDEP enforcement (or originate from external tools). If any 256-pixel
# horizontal block exceeds the per-block range limit, prompt the user to
# auto-clamp; otherwise the eventual DFX/JO export will fail at the bake
# guard with a less actionable message.
func _check_loaded_cdep_violations() -> void:
	if not _heightmap_image:
		return
	var count := CDEPConstraint.count_violations(_heightmap_image)
	if count == 0:
		return
	if workstation and workstation.has_method("prompt_cdep_violations"):
		workstation.prompt_cdep_violations(count, Callable(self, "_auto_fix_cdep_violations"))
	else:
		_notify_status("Heightmap has %d area%s too steep for Joint Operations / DFX export." % [count, "" if count == 1 else "s"])


func _auto_fix_cdep_violations() -> void:
	if not _heightmap_image:
		return
	var clamped := CDEPConstraint.clamp_all_violations(_heightmap_image)
	terrain_mesh.set_heightmap(_heightmap_image)
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()
	is_dirty = true
	_notify_status("Flattened %d area%s for Joint Operations / DFX export." % [clamped, "" if clamped == 1 else "s"])


# Brush enforcement keeps newly-sculpted heightmaps inside the CDEP envelope,
# but heightmaps loaded from disk (or imported from other tools) can still
# contain blocks the user never touched. Run a global clamp on the export
# path so a JO/DFX bake never fails on legacy data the user hasn't visited
# with the brush yet. BHD/DPTH has no per-block range limit, so skip it.
func _auto_clamp_for_export_if_needed(flavor: int) -> void:
	if flavor != ExportFlavor.DFX_JO:
		return
	if not _heightmap_image:
		return
	var clamped := CDEPConstraint.clamp_all_violations(_heightmap_image)
	if clamped == 0:
		return
	terrain_mesh.set_heightmap(_heightmap_image)
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()
	is_dirty = true
	_notify_status("Flattened %d area%s before export." % [clamped, "" if clamped == 1 else "s"])


func begin_export_terrain(output_dir: String, flavor: int = ExportFlavor.DFX_JO) -> Error:
	if is_export_running():
		return ERR_BUSY

	DirAccess.make_dir_recursive_absolute(output_dir)
	var name := _get_terrain_name()
	_auto_clamp_for_export_if_needed(flavor)
	var raw16 := _image_to_raw16(_heightmap_image, flavor == ExportFlavor.DFX_JO)
	if raw16.is_empty():
		_notify_status("Export blocked: some areas are too steep for Joint Operations / DFX. Flatten them, or export for original Delta Force instead.")
		return ERR_INVALID_DATA
	var builder: NovaTerrainBuilder = NovaTerrainBuilder.new()
	var job: NovaTerrainBuildJob = builder.begin_build_from_data(raw16, output_dir, name, "", flavor)
	if job == null:
		return ERR_CANT_CREATE

	_export_job = job
	_export_output_dir = output_dir
	brush_active = false
	_stroke_has_last_hit = false
	_remember_export_dir(output_dir)

	if workstation and workstation.has_method("on_export_started"):
		workstation.on_export_started(output_dir)

	return OK


func export_terrain(output_dir: String, flavor: int = ExportFlavor.DFX_JO) -> Error:
	if is_export_running():
		return ERR_BUSY

	DirAccess.make_dir_recursive_absolute(output_dir)
	_auto_clamp_for_export_if_needed(flavor)
	var raw16 := _image_to_raw16(_heightmap_image, flavor == ExportFlavor.DFX_JO)
	if raw16.is_empty():
		_notify_status("Export blocked: some areas are too steep for Joint Operations / DFX. Flatten them, or export for original Delta Force instead.")
		return ERR_INVALID_DATA
	var builder: NovaTerrainBuilder = NovaTerrainBuilder.new()
	var err := builder.build_from_data(raw16, output_dir, _get_terrain_name(), "", flavor)
	if err != OK:
		return err

	var name := _get_terrain_name()
	err = _save_texture_assets(output_dir, name)
	if err != OK:
		return err
	err = _document.save_tileinfo(output_dir, name)
	if err != OK:
		return err

	_document.prepare_data_for_trn_save(name, name + ".cpt")
	err = ResourceSaver.save(_data, output_dir + "/" + name + ".trn")
	if err != OK:
		return err

	_remember_export_dir(output_dir)
	_sync_hud_from_editor()
	return OK


func _poll_export_job() -> void:
	if _export_job == null:
		return
	if not _export_job.is_finished():
		return
	_finish_export_job()


func _finish_export_job() -> void:
	var job: NovaTerrainBuildJob = _export_job
	if job == null:
		return

	job.wait_for_completion()
	var err: Error = job.get_result_error()
	var message: String = job.get_result_message()
	var output_dir: String = _export_output_dir
	var name: String = _get_terrain_name()

	if err == OK:
		err = _save_texture_assets(output_dir, name)
		if err == OK:
			err = _document.save_tileinfo(output_dir, name)
		if err == OK:
			_document.prepare_data_for_trn_save(name, name + ".cpt")
			err = ResourceSaver.save(_data, output_dir + "/" + name + ".trn")
		if err == OK:
			message = "Exported to: %s" % output_dir
		elif message.is_empty():
			message = "Export failed (error %d)" % err
	elif message.is_empty():
		message = "Export failed (error %d)" % err

	_export_job = null
	_export_output_dir = ""

	if workstation and workstation.has_method("on_export_completed"):
		workstation.on_export_completed(err, message)
	_sync_hud_from_editor()


func _create_default_document(terrain_name: String) -> void:
	_document.create_default_document(terrain_name, _get_material(), _build_default_sector_grid())
	_active_sector_cell = Vector2i(-1, -1)
	selected_surface_index = TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX
	_update_hud()


func _apply_default_visual_state(sync_data: bool = true) -> void:
	_document.apply_default_visual_state(_get_material(), sync_data)


func _apply_loaded_textures_from_data() -> void:
	_document.apply_loaded_textures_from_data(_get_material())

func _save_texture_assets(output_dir: String, terrain_name: String) -> Error:
	return _document.save_texture_assets(_get_material(), output_dir, terrain_name)

func _build_heightmap_from_data() -> Image:
	return _document.build_heightmap_from_data()


func _build_heightmap_from_raw16(raw_bytes: PackedByteArray) -> Image:
	return _document.build_heightmap_from_raw16(raw_bytes)


func _log_image_stats(tag: String, image: Image) -> void:
	var data := image.get_data()
	var count := HM_SIZE * HM_SIZE
	var lo := INF
	var hi := -INF
	var sum := 0.0
	for i in count:
		var v := data.decode_float(i * 4)
		lo = minf(lo, v)
		hi = maxf(hi, v)
		sum += v
	print("[height-debug] %s image stats: min=%.4f max=%.4f avg=%.4f" % [tag, lo, hi, sum / float(count)])


func _set_heightmap_image(image: Image) -> void:
	_document.set_heightmap_image(image)
	terrain_mesh.set_heightmap(_heightmap_image)
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()


func _set_colormap_image(image: Image, sync_data: bool = true) -> void:
	_document.set_colormap_image(_get_material(), image, sync_data)


func _set_blendmap_image(image: Image, sync_data: bool = true) -> void:
	_document.set_blendmap_image(_get_material(), image, sync_data)


func _sync_material_from_data() -> void:
	_document.sync_material_from_data(_get_material())


func _sync_sector_layout(reframe_camera: bool) -> void:
	if not _data:
		return
	terrain_mesh.set_sector_layout(
		_data.get_sector_count(),
		_data.get_sector_rows(),
		_data.get_sector_grid(),
		_data.get_origin_x(),
		_data.get_origin_y()
	)
	_sync_material_from_data()
	_update_water_plane()
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()
	if reframe_camera:
		_frame_camera_to_terrain()
	_update_hud()


func _is_brush_preview_tool() -> bool:
	return current_tool != Tool.EDIT_SECTORS and current_tool != Tool.TILE_STAMP


func _frame_camera_to_terrain() -> void:
	var bounds := terrain_mesh.get_world_bounds()
	if bounds.size.x <= 0.0 or bounds.size.z <= 0.0:
		return
	# Prefer the authored sector region over the full grid extent — many
	# terrains only populate a handful of sectors out of the 8×8 grid, and
	# fitting the full empty extent puts the camera well past the far
	# plane for nothing. Fall back to the full bounds if no sectors are
	# authored yet.
	var authored_rect := _authored_sector_world_rect()
	var center: Vector3
	var extent: float
	if authored_rect.size.x > 0.0 and authored_rect.size.y > 0.0:
		center = Vector3(
			authored_rect.position.x + authored_rect.size.x * 0.5,
			bounds.position.y + bounds.size.y * 0.5,
			authored_rect.position.y + authored_rect.size.y * 0.5)
		extent = maxf(authored_rect.size.x, authored_rect.size.y)
	else:
		center = bounds.position + bounds.size * 0.5
		extent = maxf(bounds.size.x, bounds.size.z)
	if camera and camera.has_method("frame_bounds"):
		camera.frame_bounds(center, extent)


# Returns the XZ-plane AABB covering sectors that the grid marks as authored
# (cell value != 0). Rect2.size is zero when nothing is authored.
func _authored_sector_world_rect() -> Rect2:
	var sector_count: int = terrain_mesh.get_sector_count()
	var sector_rows: int = terrain_mesh.get_sector_rows()
	if sector_count <= 0 or sector_rows <= 0:
		return Rect2()
	var sector_size: float = terrain_mesh.SECTOR_SIZE
	var origin_x: float = float(_data.get_origin_x()) if _data else 0.0
	var origin_y: float = float(_data.get_origin_y()) if _data else 0.0
	var min_col := sector_count
	var max_col := -1
	var min_row := sector_rows
	var max_row := -1
	for row in sector_rows:
		for col in sector_count:
			if terrain_mesh.get_sector_cell_value(row, col) != 0:
				min_col = mini(min_col, col)
				max_col = maxi(max_col, col)
				min_row = mini(min_row, row)
				max_row = maxi(max_row, row)
	if max_col < min_col or max_row < min_row:
		return Rect2()
	var world_x := (origin_x + float(min_col)) * sector_size
	var world_z := (origin_y + float(min_row)) * sector_size
	var width := float(max_col - min_col + 1) * sector_size
	var height := float(max_row - min_row + 1) * sector_size
	return Rect2(world_x, world_z, width, height)


func _build_default_sector_grid() -> PackedInt32Array:
	var grid := PackedInt32Array()
	grid.resize(256)
	for row in 8:
		for col in 8:
			grid[row * 16 + col] = DEFAULT_SECTOR_PATTERN[row * 8 + col]
	return grid


func _create_heightmap_image(fill_height: float) -> Image:
	var image := Image.create(HM_SIZE, HM_SIZE, false, Image.FORMAT_RF)
	image.fill(Color(fill_height, 0, 0, 1))
	return image


func _create_color_image(width: int, height: int, color: Color) -> Image:
	var image := Image.create(width, height, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image


func _get_terrain_name() -> String:
	return _document.get_terrain_name()


func _get_material() -> ShaderMaterial:
	if terrain_mesh == null:
		return null
	return terrain_mesh.get_material()


func _image_to_raw16(image: Image, enforce_cdep: bool = false) -> PackedByteArray:
	return _document.image_to_raw16(image, enforce_cdep)


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		request_quit_editor()
