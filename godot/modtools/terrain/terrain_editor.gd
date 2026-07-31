class_name TerrainEditor
extends Node3D

signal ui_state_changed(version: int)

enum Tool { RAISE, LOWER, SMOOTH, FLATTEN, PAINT_DETAIL, EDIT_SECTORS, PAINT_COLORMAP, CLONE_COLOR, TILE_STAMP, FOLIAGE_PAINT, SURFACE_PAINT }
enum TileInteractionMode { PLACE, EDIT_SELECTED }

# Maps to opennova::DepthFormat in libs/cpt/include/cpt/cpt.h.
# BHD-era terrains (DVD4, original DPTH golden) use DPTH; JO/DFX-era use CDEP.
enum ExportFlavor { BHD = 0, DFX_JO = 1 }

const HM_SIZE := NovaTerrainData.ATLAS_SIZE
const DEFAULT_HEIGHT := 20.0
const INVALID_HEIGHT := -1000000.0
const INVALID_HIT := Vector3(INF, INF, INF)
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditorSurfacePaint = preload("res://modtools/terrain/terrain_editor_surface_paint.gd")
const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const TerrainEditorDocument = preload("res://modtools/terrain/terrain_editor_document.gd")
const TerrainEditorBrushSession = preload("res://modtools/terrain/terrain_editor_brush_session.gd")
const TerrainFoliagePreview = preload("res://modtools/terrain/terrain_foliage_preview.gd")
const TerrainTileOverlayPreview = preload("res://modtools/terrain/terrain_tile_overlay_preview.gd")
const WorldContextPreview = preload("res://modtools/framework/world_context_preview.gd")
const TerrainEditorTileinfoOps = preload("res://modtools/terrain/terrain_editor_tileinfo_ops.gd")
const TerrainEditorFoliageOps = preload("res://modtools/terrain/terrain_editor_foliage_ops.gd")
const TerrainEditorIoOps = preload("res://modtools/terrain/terrain_editor_io_ops.gd")
const TerrainEditorBrushOps = preload("res://modtools/terrain/terrain_editor_brush_ops.gd")
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

## User-visible Terrain tile authoring layers. TerrainEditor owns this typed
## snapshot so callers never learn the preview child's node structure.
class TileOverlayPreviewDiagnostics:
	extends RefCounted
	var base_overlay_visible: bool = false
	var outline_visible: bool = false
	var selection_visible: bool = false
	var ghost_visible: bool = false

@onready var terrain_world_root: Node3D = $TerrainWorldRoot
@onready var terrain_mesh: EditorTerrainMesh = $TerrainWorldRoot/EditorTerrainMesh
@onready var camera: Camera3D = $TerrainWorldRoot/FlyCamera

# The shell, injected by the app root (EditorApp) before set_editor; null in
# headless tests, so every use guards.
var workstation: Node = null

var _document: TerrainEditorDocument = TerrainEditorDocument.new()
var _brush_session: TerrainEditorBrushSession = TerrainEditorBrushSession.new()

# Method-bundle sections (W4-6d): operation clusters extracted off the
# facade. ALL state stays here on the mount; sections reach back via `_te`.
var _tileinfo_ops  # TerrainEditorTileinfoOps (created in _init)
var _foliage_ops  # TerrainEditorFoliageOps (created in _init)
var _io_ops  # TerrainEditorIoOps (created in _init)
var _brush_ops  # TerrainEditorBrushOps (created in _init)


func _init() -> void:
	_tileinfo_ops = TerrainEditorTileinfoOps.new(self)
	_foliage_ops = TerrainEditorFoliageOps.new(self)
	_io_ops = TerrainEditorIoOps.new(self)
	_brush_ops = TerrainEditorBrushOps.new(self)


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

var _export_job: NovaTerrainBuildJob
var _export_output_dir: String = ""

var _clone_source_marker: MeshInstance3D
# World-origin axes gizmo for the shell's View > Show axes toggle (the
# shows_view_guides hook). Hidden by default; the shell pushes its persisted
# state on every workspace activation.
var _axes_gizmo: MeshInstance3D

# The in-world preview furniture (environment/sky/weather/water under the
# world root) lives in the shared WorldContextPreview service; built in _ready
# with this editor's seam lambdas.
var _world_preview: WorldContextPreview
var water_visible: bool = true
var sector_overlay_visible: bool = false
# The shell's View > Show grid guide: thin neutral sector-boundary lines
# (u_show_grid). Owned by the View toggle alone — the Layout inspector's
# checkbox owns sector_overlay_visible (the colored diagnostic) and the two
# never share state.
var grid_guide_visible: bool = false

# The app-owned environment DOCUMENT handle. Mount-owned on purpose — the shell,
# boot probe, and tests read or assign it directly; set_environment_editor
# routes the world-preview binding through _world_preview.
var environment_editor

var _foliage_preview: TerrainFoliagePreview
var _tile_overlay_preview: TerrainTileOverlayPreview
# Mission workspace presentation context. The authored Terrain document stays
# untouched; these values only override what the shared preview presents while
# Mission is active, then clear atomically on workspace exit.
var _mission_preview_context_active := false
var _mission_preview_tile_info: NovaTerrainTileInfo
var _mission_preview_time_of_day := NAN
# Identity guard: allocation-heavy full preprocessing runs once per terrain
# mount; edit transactions call the narrow refresh family below.
var _surface_inputs_data: NovaTerrainData
var _tile_interaction_mode: int = TileInteractionMode.PLACE
var _surface_map_stroke_before: Dictionary = {}
var _surface_map_stroke_changed: bool = false
var _foliage_map_stroke_before: Dictionary = {}
var _foliage_map_stroke_changed: bool = false

var is_dirty: bool:
	get:
		return _document.is_dirty
	set(value):
		if _document.is_dirty == value:
			return
		_document.is_dirty = value
		_mark_ui_state_changed()

# Monotonic count of terrain HEIGHT changes — strokes, undo/redo of height
# snapshots, and every full heightmap replacement (new/open/import). Blendmap/
# colormap/foliage-only edits never bump it. The Mission workspace captures it
# at load and compares on reconcile to detect that placed objects may have
# drifted off the ground (editor-depth roadmap, re-ground mechanic).
var _height_revision := 0


func get_height_revision() -> int:
	return _height_revision

var _last_open_dir: String = ""
var _last_save_dir: String = ""
var _last_export_dir: String = ""
var _ui_state_version: int = 0
var _uses_workspace_viewport: bool = false
var _viewport_active: bool = false
var _viewport_edit_input_active: bool = false
var _viewport_mouse_position: Vector2 = Vector2.ZERO


func _ready() -> void:
	# World furniture and persisted state only. The app root (EditorApp._ready,
	# which runs after this) injects the environment document + workstation,
	# binds the shell, boots MCP, and seeds the initial terrain.
	camera.position = Vector3(512, 80, 600)
	camera.rotation_degrees = Vector3(-30, 0, 0)
	# World furniture: the shared WorldContextPreview service owns the
	# environment/sky/weather/water nodes. The lambdas capture this editor's
	# document/mesh reads so the service never reaches back into the mount.
	_world_preview = WorldContextPreview.new(
		terrain_world_root,
		func() -> ShaderMaterial: return _get_material(),
		func() -> float: return float(get_water_height()),
		func(world_x: float, world_z: float) -> float: return sample_height_world(world_x, world_z),
		func() -> void: _on_environment_state_changed()
	)
	_world_preview.init_environment_preview()
	_world_preview.init_water_plane()
	_init_foliage_preview()
	_init_tile_overlay_preview()
	_init_clone_marker()
	_init_axes_gizmo()
	if camera.has_signal("escape_pressed"):
		camera.connect("escape_pressed", Callable(self, "_on_camera_escape"))
	# One document for the editor's life (field-initialized, never reassigned).
	_document.error_reported.connect(_on_document_error)
	_load_editor_state()


func set_workstation(value: Node) -> void:
	workstation = value


## Forward a short status message to the workstation UI.
func _notify_status(message: String, severity: StringName = &"info") -> void:
	if workstation and workstation.has_method("show_status_message"):
		workstation.show_status_message(message, 0.0, severity)


## The document's rejected-input reports (wrong-format texture picks) become
## shell toasts; the console line stays at the source.
func _on_document_error(message: String) -> void:
	_notify_status(message, &"error")


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
	terrain_world_root.add_child(_clone_source_marker)


# X red / Y green / Z blue line gizmo at the world origin, unshaded and
# depth-free (the clone-marker material recipe) so it reads over sculpted
# heights. The surface-following sector overlay serves as the grid guide; this
# covers the axes half of shows_view_guides.
func _init_axes_gizmo() -> void:
	var axis_length := 64.0
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	mat.no_depth_test = true
	var mesh := ImmediateMesh.new()
	mesh.surface_begin(Mesh.PRIMITIVE_LINES, mat)
	mesh.surface_set_color(Color(0.95, 0.25, 0.25))
	mesh.surface_add_vertex(Vector3.ZERO)
	mesh.surface_add_vertex(Vector3(axis_length, 0.0, 0.0))
	mesh.surface_set_color(Color(0.35, 0.9, 0.35))
	mesh.surface_add_vertex(Vector3.ZERO)
	mesh.surface_add_vertex(Vector3(0.0, axis_length, 0.0))
	mesh.surface_set_color(Color(0.3, 0.55, 1.0))
	mesh.surface_add_vertex(Vector3.ZERO)
	mesh.surface_add_vertex(Vector3(0.0, 0.0, axis_length))
	mesh.surface_end()
	_axes_gizmo = MeshInstance3D.new()
	_axes_gizmo.name = "AxesGizmo"
	_axes_gizmo.mesh = mesh
	_axes_gizmo.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_axes_gizmo.visible = false
	terrain_world_root.add_child(_axes_gizmo)


func is_axes_visible() -> bool:
	return _axes_gizmo != null and _axes_gizmo.visible


func set_axes_visible(visible: bool) -> void:
	if _axes_gizmo != null:
		_axes_gizmo.visible = visible


func _init_tile_overlay_preview() -> void:
	_tile_overlay_preview = TerrainTileOverlayPreview.new()
	_tile_overlay_preview.name = "TileOverlayPreview"
	terrain_world_root.add_child(_tile_overlay_preview)


func _init_foliage_preview() -> void:
	_foliage_preview = TerrainFoliagePreview.new()
	_foliage_preview.name = "FoliagePreview"
	terrain_world_root.add_child(_foliage_preview)


## Wire the app-owned environment document into the world preview. The document
## var stays on the mount (duck-typed consumers and tests assign it directly);
## the service owns the signal binding and the node fan-out.
func set_environment_editor(value) -> void:
	environment_editor = value
	if _world_preview != null:
		_world_preview.bind_environment_editor(value)


func _on_environment_state_changed() -> void:
	if workstation and workstation.has_method("sync_from_editor_state"):
		workstation.sync_from_editor_state()


func _apply_environment_to_preview() -> void:
	if _world_preview != null:
		_world_preview.apply_environment_to_preview()


func _update_water_plane() -> void:
	var water: Node3D = _world_preview.get_water_node() if _world_preview != null else null
	if water == null:
		return
	var has_bounds := false
	if terrain_mesh:
		var bounds: AABB = terrain_mesh.get_world_bounds()
		has_bounds = bounds.size.x > 0.0 and bounds.size.z > 0.0
	# Document drives height; NovaWater follows the camera and renders the
	# env-derived lit water color + murk alpha.
	water.set_height_override(float(get_water_height()))
	water.visible = water_visible and has_bounds
	_apply_environment_to_preview()


func _unhandled_input(event: InputEvent) -> void:
	if _uses_workspace_viewport:
		return
	_handle_viewport_input(event)


func handle_viewport_input(event: InputEvent) -> void:
	if not _viewport_active or not _viewport_edit_input_active:
		return
	if event is InputEventMouse:
		_viewport_mouse_position = (event as InputEventMouse).position
	_handle_viewport_input(event)


func _handle_viewport_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed:
		var focus_owner := get_viewport().gui_get_focus_owner()
		if focus_owner != null:
			focus_owner.release_focus()

	if is_export_running():
		if event is InputEventMouseButton:
			var locked_mouse := event as InputEventMouseButton
			if locked_mouse.button_index == MOUSE_BUTTON_LEFT and not locked_mouse.pressed:
				_brush_ops._on_primary_end()
		return

	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			if mouse_button.pressed:
				_brush_ops._on_primary_start()
			else:
				_brush_ops._on_primary_end()
		elif mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed:
			if current_tool == Tool.TILE_STAMP and _tileinfo_ops.is_tile_edit_mode():
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
		# Undo/redo shortcuts live in the shell's _shortcut_input (B6).
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
				set_brush_radius_value(maxf(TerrainEditorBrushSession.BRUSH_RADIUS_MIN, brush_radius - 4.0))
			KEY_BRACKETRIGHT:
				set_brush_radius_value(minf(TerrainEditorBrushSession.BRUSH_RADIUS_MAX, brush_radius + 4.0))
			KEY_SEMICOLON:
				set_brush_strength_value(maxf(TerrainEditorBrushSession.BRUSH_STRENGTH_MIN, brush_strength - 0.05))
			KEY_APOSTROPHE:
				set_brush_strength_value(minf(TerrainEditorBrushSession.BRUSH_STRENGTH_MAX, brush_strength + 0.05))
			KEY_COMMA:
				set_brush_hardness_value(maxf(TerrainEditorBrushSession.BRUSH_HARDNESS_MIN, brush_hardness - 0.05))
			KEY_PERIOD:
				set_brush_hardness_value(minf(TerrainEditorBrushSession.BRUSH_HARDNESS_MAX, brush_hardness + 0.05))


func _process(delta: float) -> void:
	_io_ops._poll_export_job()
	if _uses_workspace_viewport and not _viewport_active:
		return
	if _world_preview != null:
		_world_preview.sync_environment_to_preview()

	if not _viewport_edit_input_active and _uses_workspace_viewport:
		_hover_hit = INVALID_HIT
		_hover_hit_valid = false
		var inactive_material := _get_material()
		if inactive_material:
			inactive_material.set_shader_parameter("u_brush_pos", Vector2(-10000.0, -10000.0))
			inactive_material.set_shader_parameter("u_show_surface_overlay", false)
		_foliage_ops._sync_foliage_preview()
		_tileinfo_ops._sync_tile_overlay_preview()
		return

	_hover_hit = _brush_ops._raycast_terrain()
	_hover_hit_valid = _brush_ops._is_valid_hit(_hover_hit)

	var material := _get_material()
	if _is_brush_preview_tool() and _hover_hit_valid:
		material.set_shader_parameter("u_brush_pos", Vector2(_hover_hit.x, _hover_hit.z))
		material.set_shader_parameter("u_brush_radius", brush_radius)
		material.set_shader_parameter("u_brush_hardness", brush_hardness)
	else:
		material.set_shader_parameter("u_brush_pos", Vector2(-10000.0, -10000.0))
	_sync_surface_overlay_state(material)

	if brush_active and _is_brush_preview_tool():
		_brush_ops._apply_brush_stroke(delta)

	_foliage_ops._sync_foliage_preview()
	_tileinfo_ops._sync_tile_overlay_preview()


func set_tool(tool: Tool) -> void:
	if is_export_running():
		return
	if current_tool == Tool.TILE_STAMP and tool != Tool.TILE_STAMP and has_selected_tileinfo_entry():
		_document.clear_tileinfo_selection()
		_mark_tile_overlay_dirty()
	if tool != Tool.TILE_STAMP:
		_tile_interaction_mode = TileInteractionMode.PLACE
	if current_tool == tool:
		return
	if brush_active:
		_brush_ops._on_primary_end()
	current_tool = tool
	flatten_target_set = false
	_sync_surface_overlay_state(_get_material())
	_mark_ui_state_changed()


func _sync_surface_overlay_state(material: ShaderMaterial) -> void:
	if material == null:
		return
	var show_authoring := not _mission_preview_context_active
	var overlay_enabled := show_authoring and current_tool == Tool.SURFACE_PAINT
	var preview_color := Color(1.0, 1.0, 0.2)
	if overlay_enabled:
		var preview_index := _get_surface_paint_index(Input.is_key_pressed(KEY_CTRL))
		preview_color = TerrainEditorSurfacePaint.get_surface_color(preview_index, _document.get_surface_palette_bytes())
	material.set_shader_parameter("u_show_surface_overlay", overlay_enabled)
	material.set_shader_parameter("u_show_sector_overlay", show_authoring and sector_overlay_visible)
	material.set_shader_parameter("u_show_grid", show_authoring and grid_guide_visible)
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
	_mark_ui_state_changed()


func get_paint_color() -> Color:
	return paint_color


func get_terrain_name_value() -> String:
	return _document.get_terrain_name()


func get_detail_density() -> int:
	return _data.get_detail_density() if _data else 0


func set_detail_density_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	if _data.get_detail_density() == value:
		return
	_data.set_detail_density(value)
	_document.sync_material_from_data(_get_material())
	is_dirty = true
	_mark_ui_state_changed()


func get_detail_density2() -> int:
	return _data.get_detail_density2() if _data else 0


func set_detail_density2_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	if _data.get_detail_density2() == value:
		return
	_data.set_detail_density2(value)
	is_dirty = true
	_mark_ui_state_changed()


func get_wrap_x_enabled() -> bool:
	return _data.get_wrap_x() if _data else false


func set_wrap_x_enabled(enabled: bool) -> void:
	if is_export_running() or not _data:
		return
	if _data.get_wrap_x() == enabled:
		return
	_data.set_wrap_x(enabled)
	is_dirty = true
	_mark_ui_state_changed()


func get_wrap_y_enabled() -> bool:
	return _data.get_wrap_y() if _data else false


func set_wrap_y_enabled(enabled: bool) -> void:
	if is_export_running() or not _data:
		return
	if _data.get_wrap_y() == enabled:
		return
	_data.set_wrap_y(enabled)
	is_dirty = true
	_mark_ui_state_changed()

func get_data() -> NovaTerrainData:
	return _data


# World-space height of the LIVE editable surface under (world_x, world_z) — the
# same live substrate + bilinear core the placement/drag raycasts hit
# (raycast_terrain_at, via NovaTerrainData's shared sampler). NOT the baked
# CPT sampler (NovaTerrainData.get_height_world*): height brushes mutate only the
# editable image, so the baked buffer is stale the moment _height_revision moves
# (and absent entirely on never-exported project terrains). Returns NAN when no
# terrain is live or the point is off the mesh; the Mission workspace's re-ground
# skips those entities rather than grounding them to a bogus height. Duck-typed
# so the mission tests' headless stub can fake the surface.
func sample_height_world(world_x: float, world_z: float) -> float:
	if terrain_mesh == null:
		return NAN
	var height := terrain_mesh.sample_world_height(world_x, world_z)
	if height == INVALID_HEIGHT:
		return NAN
	return height


# Batch variant of sample_height_world: the live-surface height under each
# (world_x, world_z) point, NAN per off-mesh / no-terrain point — one C++ call
# for the whole set instead of one per point (the mission re-ground builds one
# request per entity). Scalar and batch run the same NovaTerrainData live
# sampler core; duck-typed so the mission tests' headless stubs can fake the
# surface (they implement this by looping their scalar fake).
func sample_heights_world(points: PackedVector2Array) -> PackedFloat32Array:
	if terrain_mesh == null:
		var out := PackedFloat32Array()
		out.resize(points.size())
		out.fill(NAN)
		return out
	return terrain_mesh.sample_world_heights(points)


func get_environment_editor():
	return environment_editor


# The shared NovaEnvironment node (EditorEnvironment under the world root). The
# mission workspace hands it to placed objects so their lighting matches the
# terrain preview, the same way the runtime passes its NovaEnvironment node.
func get_environment_node() -> Node:
	return _world_preview.get_environment_node() if _world_preview != null else null


func get_terrain_world_root() -> Node3D:
	return terrain_world_root


func set_viewport_active(active: bool, edit_input_enabled: bool = true) -> void:
	_uses_workspace_viewport = true
	var next_edit_input := active and edit_input_enabled
	if _viewport_active == active and _viewport_edit_input_active == next_edit_input:
		return
	if not next_edit_input and brush_active:
		# Workspace switches/focus loss can interrupt a held stroke without a
		# mouse-up event. Finalize through the normal path so history and the
		# temporary live blend/normal bindings are restored atomically.
		_brush_ops._on_primary_end()
	_viewport_active = active
	_viewport_edit_input_active = next_edit_input
	if camera != null:
		camera.current = active
	if not next_edit_input:
		brush_active = false
		_hover_hit = INVALID_HIT
		_hover_hit_valid = false


func is_viewport_active() -> bool:
	return _viewport_active


func is_viewport_edit_input_active() -> bool:
	return _viewport_edit_input_active


func set_viewport_mouse_position(position: Vector2) -> void:
	_viewport_mouse_position = position


func get_surface_types() -> Array:
	return TerrainEditorSurfacePaint.get_surface_types()


func get_selected_surface_index() -> int:
	return selected_surface_index


func set_selected_surface_index(value: int) -> void:
	var next := clampi(value, 0, 255)
	if selected_surface_index == next:
		return
	selected_surface_index = next
	_mark_ui_state_changed()


func get_selected_surface_label() -> String:
	return TerrainEditorSurfacePaint.get_surface_label(selected_surface_index)


const FOLIAGE_DEFS_LIMIT := 4


func add_foliage_def() -> void:
	_foliage_ops.add_foliage_def()


func remove_foliage_def(index: int) -> void:
	_foliage_ops.remove_foliage_def(index)


func set_foliage_def_field(index: int, field: String, value: Variant) -> void:
	_foliage_ops.set_foliage_def_field(index, field, value)


func save_project_to_current_dir() -> Error:
	if _document.current_project_dir.is_empty():
		return ERR_INVALID_PARAMETER
	return save_project(_document.current_project_dir)


func get_paint_detail_channel() -> int:
	return paint_detail_channel


func set_brush_radius_value(value: float) -> void:
	if is_export_running():
		return
	var next := clampf(value, TerrainEditorBrushSession.BRUSH_RADIUS_MIN, TerrainEditorBrushSession.BRUSH_RADIUS_MAX)
	if is_equal_approx(brush_radius, next):
		return
	brush_radius = next
	_mark_ui_state_changed()


func set_brush_strength_value(value: float) -> void:
	if is_export_running():
		return
	var next := clampf(value, TerrainEditorBrushSession.BRUSH_STRENGTH_MIN, TerrainEditorBrushSession.BRUSH_STRENGTH_MAX)
	if is_equal_approx(brush_strength, next):
		return
	brush_strength = next
	_mark_ui_state_changed()


func set_brush_hardness_value(value: float) -> void:
	if is_export_running():
		return
	var next := clampf(value, TerrainEditorBrushSession.BRUSH_HARDNESS_MIN, TerrainEditorBrushSession.BRUSH_HARDNESS_MAX)
	if is_equal_approx(brush_hardness, next):
		return
	brush_hardness = next
	_mark_ui_state_changed()


func get_sector_count() -> int:
	return _data.get_sector_count() if _data else 0


func get_sector_size() -> int:
	return maxi(get_sector_count(), get_sector_rows())


func get_sector_rows() -> int:
	return _data.get_sector_rows() if _data else 0


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
	if _data.get_origin_x() == value:
		return
	_data.set_origin_x(value)
	_sync_sector_layout(true)
	is_dirty = true


func get_origin_y() -> int:
	return _data.get_origin_y() if _data else 0


func set_origin_y_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	if _data.get_origin_y() == value:
		return
	_data.set_origin_y(value)
	_sync_sector_layout(true)
	is_dirty = true


func get_water_height() -> int:
	return int(_data.get_water_height() / 2) if _data else 0


func set_water_height_value(value: int) -> void:
	if is_export_running() or not _data:
		return
	if get_water_height() == value:
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
	_mark_ui_state_changed()


func is_sector_overlay_visible() -> bool:
	return sector_overlay_visible


func set_sector_overlay_visible(visible: bool) -> void:
	if sector_overlay_visible == visible:
		return
	sector_overlay_visible = visible
	if terrain_mesh:
		_sync_surface_overlay_state(_get_material())
	_mark_ui_state_changed()


func is_grid_guide_visible() -> bool:
	return grid_guide_visible


func set_grid_guide_visible(visible: bool) -> void:
	if grid_guide_visible == visible:
		return
	grid_guide_visible = visible
	if terrain_mesh:
		_sync_surface_overlay_state(_get_material())
	_mark_ui_state_changed()


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


func get_resource_root() -> NovaResourceRoot:
	if workstation != null and workstation.has_method("get_resource_root"):
		return workstation.get_resource_root()
	return null


func set_sector_cell(row: int, col: int, value: int) -> bool:
	if is_export_running() or not _data:
		return false
	var idx := row * NovaTerrainData.SECTOR_GRID_DIM + col
	var grid: PackedInt32Array = _data.get_sector_grid()
	if idx < 0 or idx >= grid.size():
		return false
	var next := clampi(value, 0, NovaTerrainData.SECTOR_ID_MAX)
	_active_sector_cell = Vector2i(row, col)
	if grid[idx] == next:
		_mark_ui_state_changed()
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


func get_selected_foliage_def_index() -> int:
	return _document.selected_foliage_def_index


func set_selected_foliage_def_index(index: int) -> void:
	var before := _document.selected_foliage_def_index
	_document.set_selected_foliage_def_index(index)
	if _document.selected_foliage_def_index == before:
		return
	_mark_foliage_preview_dirty()
	_mark_ui_state_changed()


func get_selected_foliage_def() -> NovaTerrainFoliageDef:
	return _document.get_selected_foliage_def()


func has_tileinfo_resource() -> bool:
	return _document.has_tileinfo_resource()


func get_tileinfo_entry(index: int) -> NovaTerrainTileEntry:
	return _document.get_tileinfo_entry(index)


func get_tileinfo_selected_index() -> int:
	return _document.get_tileinfo_selected_index()


func has_selected_tileinfo_entry() -> bool:
	return get_selected_tileinfo_entry() != null


func get_selected_tileinfo_entry() -> NovaTerrainTileEntry:
	return _document.get_tileinfo_entry(_document.get_tileinfo_selected_index())


func get_selected_tileinfo_world_center() -> Vector3:
	var entry := get_selected_tileinfo_entry()
	if entry == null:
		return Vector3.ZERO
	return TerrainTileOverlayPreview.entry_center_world(entry, terrain_mesh)


func clear_tileinfo_selection() -> void:
	_tileinfo_ops.clear_tileinfo_selection()


func select_tileinfo_entry(index: int, focus_camera: bool = false) -> void:
	_tileinfo_ops.select_tileinfo_entry(index, focus_camera)


func get_tile_stamp_tile_index() -> int:
	return _document.get_tile_stamp_tile_index()


func set_tile_stamp_tile_index(value: int) -> void:
	_tileinfo_ops.set_tile_stamp_tile_index(value)


func get_tile_stamp_flags() -> int:
	return _document.get_tile_stamp_flags()


func set_tile_stamp_flags(value: int) -> void:
	_tileinfo_ops.set_tile_stamp_flags(value)


func stamp_tileinfo_cell(cell_x: int, cell_z: int) -> bool:
	return _tileinfo_ops.stamp_tileinfo_cell(cell_x, cell_z)


func delete_selected_tileinfo_entry() -> bool:
	return _tileinfo_ops.delete_selected_tileinfo_entry()


func replace_selected_tileinfo_tile_index(value: int) -> bool:
	return _tileinfo_ops.replace_selected_tileinfo_tile_index(value)


func set_selected_tileinfo_flags(value: int) -> bool:
	return _tileinfo_ops.set_selected_tileinfo_flags(value)


func rotate_selected_tileinfo_clockwise() -> bool:
	return _tileinfo_ops.rotate_selected_tileinfo_clockwise()


func flip_selected_tileinfo_x() -> bool:
	return _tileinfo_ops.flip_selected_tileinfo_x()


func flip_selected_tileinfo_y() -> bool:
	return _tileinfo_ops.flip_selected_tileinfo_y()


func load_texture_slot(slot_id: String, path: String) -> void:
	_io_ops.load_texture_slot(slot_id, path)


func reset_texture_slot(slot_id: String) -> void:
	_io_ops.reset_texture_slot(slot_id)


func load_tileinfo(path: String) -> void:
	_tileinfo_ops.load_tileinfo(path)


func new_tileinfo() -> void:
	_tileinfo_ops.new_tileinfo()


func reset_tileinfo() -> void:
	_tileinfo_ops.reset_tileinfo()


## Scope the Mission workspace's external tile blockers onto the shared editor
## preview. This never mutates the Terrain document, so clearing the context
## restores its authored tile layout and UI. (Foliage silhouettes take no
## mission anchors: retail generates them only around crouched/prone infantry
## [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded].)
func set_mission_preview_context(
	tile_info: NovaTerrainTileInfo,
	preview_time_of_day: float = NAN
) -> void:
	var time_changed := is_nan(_mission_preview_time_of_day) != is_nan(preview_time_of_day) \
		or (not is_nan(preview_time_of_day) \
		and not is_equal_approx(_mission_preview_time_of_day, preview_time_of_day))
	var context_changed := not _mission_preview_context_active \
		or _mission_preview_tile_info != tile_info \
		or time_changed
	_mission_preview_context_active = true
	_mission_preview_tile_info = tile_info
	_mission_preview_time_of_day = preview_time_of_day
	_sync_surface_overlay_state(_get_material())
	if not context_changed:
		return
	if _world_preview != null:
		if is_nan(_mission_preview_time_of_day):
			_world_preview.clear_time_of_day_override()
		else:
			_world_preview.set_time_of_day_override(_mission_preview_time_of_day)
	_brush_ops._refresh_surface_input_tile_overlay()
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()


func clear_mission_preview_context() -> void:
	if not _mission_preview_context_active \
			and _mission_preview_tile_info == null \
			and is_nan(_mission_preview_time_of_day):
		return
	_mission_preview_context_active = false
	_mission_preview_tile_info = null
	_mission_preview_time_of_day = NAN
	_sync_surface_overlay_state(_get_material())
	if _world_preview != null:
		_world_preview.clear_time_of_day_override()
	_brush_ops._refresh_surface_input_tile_overlay()
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()


func is_mission_preview_context_active() -> bool:
	return _mission_preview_context_active


## GameWorld uses the co-named mission .til when present and otherwise falls
## back to the terrain-authored tile array. Keep that exact precedence in ONED.
func get_effective_tile_info() -> NovaTerrainTileInfo:
	if _mission_preview_context_active and _mission_preview_tile_info != null:
		return _mission_preview_tile_info
	return _document.tileinfo_resource

## Render-layer facts for diagnostics and integration tests. Keep ownership of
## the preview child private while exposing the user-visible layer state.
func get_tile_overlay_preview_diagnostics() -> TileOverlayPreviewDiagnostics:
	var diagnostics := TileOverlayPreviewDiagnostics.new()
	if _tile_overlay_preview == null:
		return diagnostics
	diagnostics.base_overlay_visible = _tile_overlay_preview.is_base_overlay_visible()
	diagnostics.outline_visible = _tile_overlay_preview.is_outline_visible()
	diagnostics.selection_visible = _tile_overlay_preview.is_selection_visible()
	diagnostics.ghost_visible = _tile_overlay_preview.is_ghost_visible()
	return diagnostics


func _mark_foliage_preview_dirty() -> void:
	if _foliage_preview:
		_foliage_preview.mark_dirty()


func _mark_tile_overlay_dirty() -> void:
	if _tile_overlay_preview:
		_tile_overlay_preview.mark_dirty()


func clear_clone_source() -> void:
	_brush_ops.clear_clone_source()


func has_clone_source() -> bool:
	return _brush_ops.has_clone_source()


func raycast_terrain_at(mouse_pos: Vector2) -> Vector3:
	return _brush_ops.raycast_terrain_at(mouse_pos)


func is_valid_terrain_hit(hit: Vector3) -> bool:
	return _brush_ops.is_valid_terrain_hit(hit)


func undo() -> void:
	_brush_ops.undo()


func redo() -> void:
	_brush_ops.redo()


func can_undo() -> bool:
	return _brush_ops.can_undo()


func can_redo() -> bool:
	return _brush_ops.can_redo()


func _apply_history_snapshot(snapshot: Dictionary, is_undo: bool) -> void:
	_brush_ops._apply_history_snapshot(snapshot, is_undo)


func _mark_ui_state_changed() -> void:
	_ui_state_version += 1
	ui_state_changed.emit(_ui_state_version)
	if workstation and workstation.has_method("sync_from_editor_state"):
		workstation.sync_from_editor_state()


func get_current_project_dir() -> String:
	return _document.current_project_dir


func get_current_trn_path() -> String:
	return _document.current_trn_path


func has_current_project_dir() -> bool:
	return not _document.current_project_dir.is_empty()


func get_last_open_dir() -> String:
	return _last_open_dir


func get_last_save_dir() -> String:
	return _last_save_dir


func get_last_export_dir() -> String:
	return _last_export_dir


# Escape asks the shell to close (the unified close guard, which lists every
# dirty workspace). The dirty-replace guard for New/Open lives on the terrain
# workspace adapter, through the shell's shared unsaved-changes dialog.
func _on_camera_escape() -> void:
	if workstation != null and workstation.has_method("request_close"):
		workstation.request_close()


func _load_editor_state() -> void:
	var config := ConfigFile.new()
	if config.load("user://terrain_editor_state.cfg") != OK:
		return
	_last_open_dir = String(config.get_value("paths", "last_open_dir", ""))
	_last_save_dir = String(config.get_value("paths", "last_save_dir", ""))
	_last_export_dir = String(config.get_value("paths", "last_export_dir", ""))


func _save_editor_state() -> void:
	var config := ConfigFile.new()
	config.load("user://terrain_editor_state.cfg")
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
	_io_ops.new_terrain()


func open_trn(trn_path: String, timeline: PerfTimeline = null) -> Error:
	return _io_ops.open_trn(trn_path, timeline)


func save_project(dir_path: String) -> Error:
	return _io_ops.save_project(dir_path)


func begin_export_terrain(output_dir: String, flavor: int = ExportFlavor.DFX_JO) -> Error:
	return _io_ops.begin_export_terrain(output_dir, flavor)


func export_terrain(output_dir: String, flavor: int = ExportFlavor.DFX_JO) -> Error:
	return _io_ops.export_terrain(output_dir, flavor)


func _set_heightmap_image(image: Image) -> void:
	_document.set_heightmap_image(image)
	terrain_mesh.set_heightmap(_heightmap_image)
	_height_revision += 1
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()


func _sync_material_from_data() -> void:
	_document.sync_material_from_data(_get_material())
	_apply_environment_to_preview()


func _sync_sector_layout(reframe_camera: bool) -> void:
	if not _data:
		return
	# The mesh forwards world->atlas coordinate queries to NovaTerrainData's C++
	# kernel, so hand it the live data alongside the layout it draws.
	terrain_mesh.set_terrain_data(_data)
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
	_mark_ui_state_changed()


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
	grid.resize(NovaTerrainData.SECTOR_GRID_DIM * NovaTerrainData.SECTOR_GRID_DIM)
	for row in 8:
		for col in 8:
			grid[row * NovaTerrainData.SECTOR_GRID_DIM + col] = DEFAULT_SECTOR_PATTERN[row * 8 + col]
	return grid


func _get_terrain_name() -> String:
	return _document.get_terrain_name()


func _get_material() -> ShaderMaterial:
	if terrain_mesh == null:
		return null
	return terrain_mesh.get_material()
