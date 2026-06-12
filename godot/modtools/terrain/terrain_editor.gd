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
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditorSurfacePaint = preload("res://modtools/terrain/terrain_editor_surface_paint.gd")
const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const TerrainEditorDocument = preload("res://modtools/terrain/terrain_editor_document.gd")
const TerrainEditorBrushSession = preload("res://modtools/terrain/terrain_editor_brush_session.gd")
const TerrainFoliagePreview = preload("res://modtools/terrain/terrain_foliage_preview.gd")
const TerrainTileOverlayPreview = preload("res://modtools/terrain/terrain_tile_overlay_preview.gd")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")
const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")
const NovaSkyScript = preload("res://engine/environment/nova_sky.gd")
const NovaWaterScript = preload("res://engine/environment/nova_water.gd")
const NovaWeatherScript = preload("res://engine/environment/nova_weather.gd")
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

@onready var terrain_world_root: Node3D = $TerrainWorldRoot
@onready var terrain_mesh: EditorTerrainMesh = $TerrainWorldRoot/EditorTerrainMesh
@onready var camera: Camera3D = $TerrainWorldRoot/FlyCamera
@onready var workstation = $CanvasLayer/EditorWorkstation

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

var _export_job: NovaTerrainBuildJob
var _export_output_dir: String = ""

var _clone_source_marker: MeshInstance3D

var _water_node: Node3D
var _weather_node: Node3D
var water_visible: bool = true
var sector_overlay_visible: bool = false

var environment_editor
var _environment_node: Node
var _sky_node: Node3D

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
var _pending_unsaved_action: Callable = Callable()
var _pending_unsaved_action_name: String = ""
var _previous_window_min_size: Vector2i = Vector2i.ZERO
var _ui_state_version: int = 0
var _uses_workspace_viewport: bool = false
var _viewport_active: bool = false
var _viewport_edit_input_active: bool = false
var _viewport_mouse_position: Vector2 = Vector2.ZERO


func _ready() -> void:
	get_tree().auto_accept_quit = false
	_configure_editor_window()
	camera.position = Vector3(512, 80, 600)
	camera.rotation_degrees = Vector3(-30, 0, 0)
	_init_environment_preview()
	_init_water_plane()
	_init_foliage_preview()
	_init_tile_overlay_preview()
	_init_clone_marker()
	if camera.has_signal("escape_pressed"):
		camera.connect("escape_pressed", Callable(self, "request_quit_editor"))
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
	terrain_world_root.add_child(_clone_source_marker)


func _init_tile_overlay_preview() -> void:
	_tile_overlay_preview = TerrainTileOverlayPreview.new()
	_tile_overlay_preview.name = "TileOverlayPreview"
	terrain_world_root.add_child(_tile_overlay_preview)


func _init_foliage_preview() -> void:
	_foliage_preview = TerrainFoliagePreview.new()
	_foliage_preview.name = "FoliagePreview"
	terrain_world_root.add_child(_foliage_preview)


func _init_water_plane() -> void:
	# Runtime parity (deferred from PR #24): the editor preview renders the same
	# NovaWater (water.gdshader, env-derived lit color) the runtime uses, instead
	# of a bespoke plane with a hardcoded color. Height stays document-driven via
	# the override hook.
	_water_node = Node3D.new()
	_water_node.name = "WaterPlane"
	_water_node.set_script(NovaWaterScript)
	_water_node.environment_path = NodePath("../EditorEnvironment")
	terrain_world_root.add_child(_water_node)
	_water_node.set_height_override(float(get_water_height()))


func _init_environment_preview() -> void:
	environment_editor = EnvironmentEditorScript.new()
	environment_editor.name = "EnvironmentEditor"
	add_child(environment_editor)

	_environment_node = Node.new()
	_environment_node.name = "EditorEnvironment"
	_environment_node.set_script(NovaEnvironmentScript)
	terrain_world_root.add_child(_environment_node)

	_sky_node = Node3D.new()
	_sky_node.name = "EditorSky"
	_sky_node.set_script(NovaSkyScript)
	_sky_node.environment_path = NodePath("../EditorEnvironment")
	terrain_world_root.add_child(_sky_node)

	# Runtime parity: the same weather smoothing that runs in-game also runs in
	# the preview, so scrubbing/playing TOD matches play. The tick is O(1) so it
	# does not affect brush perf; discrete scrubs call resync_colors() to snap.
	_weather_node = Node3D.new()
	_weather_node.name = "EditorWeather"
	_weather_node.set_script(NovaWeatherScript)
	_weather_node.environment_path = NodePath("../EditorEnvironment")
	terrain_world_root.add_child(_weather_node)

	if not environment_editor.environment_changed.is_connected(_on_environment_editor_changed):
		environment_editor.environment_changed.connect(_on_environment_editor_changed)
	if not environment_editor.state_changed.is_connected(_on_environment_state_changed):
		environment_editor.state_changed.connect(_on_environment_state_changed)
	_on_environment_editor_changed(environment_editor.env_file, environment_editor.time_of_day)


func _on_environment_state_changed() -> void:
	if workstation and workstation.has_method("sync_from_editor_state"):
		workstation.sync_from_editor_state()


func _on_environment_editor_changed(env_file: EnvFile, preview_time: float) -> void:
	if _environment_node:
		_environment_node.environment_data = env_file
		_environment_node.time_of_day = preview_time
	# A discrete TOD scrub or document edit must snap the weather smoother,
	# otherwise the preview lags behind the slider.
	if _weather_node and _weather_node.has_method("resync_colors"):
		_weather_node.resync_colors()
	_apply_environment_to_preview()
	if workstation and workstation.has_method("sync_from_editor_state"):
		workstation.sync_from_editor_state()


func _apply_environment_to_preview() -> void:
	if _environment_node == null or not _environment_node.has_method("is_loaded") or not _environment_node.is_loaded():
		return
	var material := _get_material()
	if material:
		# Same env -> terrain-uniform push the runtime uses (NovaEnvironment owns it).
		_environment_node.apply_terrain_uniforms(material)
	# Water color/height/murk now come from the NovaWater node (env-driven),
	# matching the runtime; nothing hardcoded here.


func _update_water_plane() -> void:
	if _water_node == null:
		return
	var has_bounds := false
	if terrain_mesh:
		var bounds: AABB = terrain_mesh.get_world_bounds()
		has_bounds = bounds.size.x > 0.0 and bounds.size.z > 0.0
	# Document drives height; NovaWater follows the camera and renders the
	# env-derived lit water color + murk alpha.
	_water_node.set_height_override(float(get_water_height()))
	_water_node.visible = water_visible and has_bounds
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
	_poll_export_job()
	if _uses_workspace_viewport and not _viewport_active:
		return

	if not _viewport_edit_input_active and _uses_workspace_viewport:
		_hover_hit = INVALID_HIT
		_hover_hit_valid = false
		var inactive_material := _get_material()
		if inactive_material:
			inactive_material.set_shader_parameter("u_brush_pos", Vector2(-10000.0, -10000.0))
			inactive_material.set_shader_parameter("u_show_surface_overlay", false)
		_sync_foliage_preview()
		_sync_tile_overlay_preview()
		return

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
	if current_tool == tool:
		return
	current_tool = tool
	flatten_target_set = false
	_sync_surface_overlay_state(_get_material())
	_mark_ui_state_changed()


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
	_mark_ui_state_changed()


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
	if _data.get_terrain_name() == next_value:
		return
	_data.set_terrain_name(next_value)
	is_dirty = true
	_mark_ui_state_changed()


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
# same image-backed bilinear sample the placement/drag raycasts ground on
# (EditorTerrainMesh.sample_world_height over the edited heightmap). NOT the baked
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
# for the whole set instead of ~6 boundary crossings per point (the mission
# re-ground builds one request per entity). Same live-image semantics as the
# scalar above; duck-typed so the mission tests' headless stubs can fake the
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
	return _environment_node


func get_terrain_world_root() -> Node3D:
	return terrain_world_root


func set_viewport_active(active: bool, edit_input_enabled: bool = true) -> void:
	_uses_workspace_viewport = true
	var next_edit_input := active and edit_input_enabled
	if _viewport_active == active and _viewport_edit_input_active == next_edit_input:
		return
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
	_mark_ui_state_changed()


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
	_mark_ui_state_changed()


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
	_mark_ui_state_changed()


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
	if _document.get_tileinfo_selected_index() < 0 and _tile_interaction_mode == TileInteractionMode.PLACE:
		return
	_document.clear_tileinfo_selection()
	_tile_interaction_mode = TileInteractionMode.PLACE
	_mark_tile_overlay_dirty()
	_mark_ui_state_changed()


func select_tileinfo_entry(index: int, focus_camera: bool = false) -> void:
	if _document.get_tileinfo_selected_index() == index and _tile_interaction_mode == TileInteractionMode.EDIT_SELECTED:
		if focus_camera:
			focus_selected_tileinfo_entry()
		return
	_document.set_tileinfo_selected_index(index, false)
	_tile_interaction_mode = TileInteractionMode.EDIT_SELECTED
	_mark_tile_overlay_dirty()
	_mark_ui_state_changed()
	if focus_camera:
		focus_selected_tileinfo_entry()


func get_tile_stamp_tile_index() -> int:
	return _document.get_tile_stamp_tile_index()


func set_tile_stamp_tile_index(value: int) -> void:
	if is_export_running():
		return
	if _document.get_tile_stamp_tile_index() == clampi(value, 0, 255):
		return
	_document.set_tile_stamp_tile_index(value)
	_mark_ui_state_changed()


func get_tile_stamp_flags() -> int:
	return _document.get_tile_stamp_flags()


func set_tile_stamp_flags(value: int) -> void:
	if is_export_running():
		return
	var normalized := value & (
		NovaTerrainTileInfo.FLAG_FLIP_X |
		NovaTerrainTileInfo.FLAG_FLIP_Y |
		NovaTerrainTileInfo.FLAG_ROTATE_90 |
		NovaTerrainTileInfo.FLAG_OUTLINE
	)
	if _document.get_tile_stamp_flags() == normalized:
		return
	_document.set_tile_stamp_flags(value)
	_mark_ui_state_changed()


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
		_mark_ui_state_changed()


func reset_texture_slot(slot_id: String) -> void:
	if is_export_running() or not _data:
		return
	_document.reset_texture_slot(_get_material(), slot_id)
	is_dirty = true
	if slot_id == "tilestrip":
		_mark_tile_overlay_dirty()
	elif slot_id == "foliagemap":
		_mark_foliage_preview_dirty()
	_mark_ui_state_changed()


func load_tileinfo(path: String) -> void:
	if is_export_running():
		return
	if _document.load_tileinfo(path):
		_normalize_loaded_tileinfo_if_needed()
		is_dirty = true
		_mark_tile_overlay_dirty()
		_mark_ui_state_changed()


func new_tileinfo() -> void:
	if is_export_running():
		return
	_document.new_tileinfo()
	is_dirty = true
	_mark_tile_overlay_dirty()
	_mark_ui_state_changed()


func reset_tileinfo() -> void:
	if is_export_running():
		return
	_document.reset_tileinfo()
	is_dirty = _document.is_dirty
	_mark_tile_overlay_dirty()
	_mark_ui_state_changed()


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
	_mark_ui_state_changed()
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
		_document.selected_foliage_def_index,
		_data,
		get_resource_root()
	)
	_foliage_preview.rebuild_if_needed()


func _apply_foliage_paint_stroke(delta: float) -> bool:
	if _document.foliage_map == null or not _hover_hit_valid:
		_brush_session.reset_stroke_tracking()
		return false

	var target_index := 0
	if not Input.is_key_pressed(KEY_CTRL):
		target_index = _document.get_selected_foliage_paint_index()
		if target_index < 0:
			_brush_session.reset_stroke_tracking()
			return false

	var start_hit := _brush_session.get_stroke_start_hit(_hover_hit)
	var end_hit := _hover_hit
	var dab_count := _brush_session.get_stroke_dab_count(start_hit, end_hit)
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

	_brush_session.commit_stroke_hit(end_hit)
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
	_mark_ui_state_changed()
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
	var surface_map: NovaTerrainSurfaceMap = _document.surface_map
	if surface_map == null or not _hover_hit_valid:
		_brush_session.reset_stroke_tracking()
		return false

	var map_width := surface_map.get_width()
	var map_height := surface_map.get_height()
	if map_width <= 0 or map_height <= 0:
		_brush_session.reset_stroke_tracking()
		return false

	var start_hit := _brush_session.get_stroke_start_hit(_hover_hit)
	var end_hit := _hover_hit
	var dab_count := _brush_session.get_stroke_dab_count(start_hit, end_hit)
	var radius_pixels := maxi(1, int(round(brush_radius * float(map_width) / float(HM_SIZE))))
	var target_index := _get_surface_paint_index(Input.is_key_pressed(KEY_CTRL))
	var changed := false

	for dab_idx in range(dab_count):
		var t: float = 1.0 if dab_count == 1 else float(dab_idx + 1) / float(dab_count)
		var dab_hit := start_hit.lerp(end_hit, t)
		var source := terrain_mesh.world_to_source_coords(dab_hit.x, dab_hit.z)
		if source.x < 0.0 or source.y < 0.0:
			continue
		var center_x := surface_map.map_x_from_heightmap_x(source.x)
		var center_y := surface_map.map_y_from_heightmap_y(source.y)
		if center_x < 0 or center_y < 0 or center_x >= map_width or center_y >= map_height:
			continue
		changed = surface_map.paint_circle(
			center_x,
			center_y,
			radius_pixels,
			brush_hardness,
			brush_strength,
			target_index
		) or changed

	if changed:
		_document.sync_surface_map_to_data(_get_material())
		_surface_map_stroke_changed = true

	_brush_session.commit_stroke_hit(end_hit)
	return changed


func _eyedrop_surface_at_hover() -> bool:
	var surface_map: NovaTerrainSurfaceMap = _document.surface_map
	if surface_map == null or not _hover_hit_valid:
		return false
	var map_width := surface_map.get_width()
	var map_height := surface_map.get_height()
	if map_width <= 0 or map_height <= 0:
		return false
	var source := terrain_mesh.world_to_source_coords(_hover_hit.x, _hover_hit.z)
	if source.x < 0.0 or source.y < 0.0:
		return false
	var map_x := surface_map.map_x_from_heightmap_x(source.x)
	var map_y := surface_map.map_y_from_heightmap_y(source.y)
	selected_surface_index = surface_map.get_index(map_x, map_y)
	_mark_ui_state_changed()
	return true


func _set_clone_source(world_pos: Vector3) -> void:
	_brush_session.set_clone_source(world_pos, _colormap_image)
	if _clone_source_marker:
		_clone_source_marker.position = world_pos
		_clone_source_marker.visible = true
	_mark_ui_state_changed()


func clear_clone_source() -> void:
	_brush_session.clear_clone_source()
	if _clone_source_marker:
		_clone_source_marker.visible = false
	_mark_ui_state_changed()


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
		if not _hover_hit_valid or _document.surface_map == null:
			return
		if Input.is_key_pressed(KEY_ALT):
			_eyedrop_surface_at_hover()
			return
		_surface_map_stroke_before = _document.capture_surface_map_history_state()
		_surface_map_stroke_changed = false
		brush_active = true
		_brush_session.reset_stroke_tracking()
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
		_brush_session.reset_stroke_tracking()
		_apply_foliage_paint_stroke(1.0 / 60.0)
		return
	if current_tool == Tool.CLONE_COLOR:
		if Input.is_key_pressed(KEY_CTRL) and _hover_hit_valid:
			_set_clone_source(_hover_hit)
			_mark_ui_state_changed()
			return
		if not _brush_session.has_clone_source() or not _hover_hit_valid:
			return
		if not _brush_session.prepare_clone_drag(_hover_hit, terrain_mesh):
			return
	# fall through to normal brush-active path
	if current_tool == Tool.PAINT_COLORMAP and Input.is_key_pressed(KEY_ALT) and _hover_hit_valid:
		var source := terrain_mesh.world_to_source_coords(_hover_hit.x, _hover_hit.z)
		if source.x >= 0.0:
			paint_color = _data.brush_sample_colormap(source.x, source.y)
			_mark_ui_state_changed()
		return
	_brush_session.begin_brush_drag(_source_image_for_kind(_brush_session.history_kind_for_tool(current_tool)), Input.is_key_pressed(KEY_CTRL))


func _on_primary_end() -> void:
	if current_tool == Tool.SURFACE_PAINT:
		brush_active = false
		_brush_session.reset_stroke_tracking()
		if _surface_map_stroke_changed:
			var after_state := _document.capture_surface_map_history_state()
			_push_surface_map_history(_surface_map_stroke_before, after_state)
			is_dirty = true
			_mark_ui_state_changed()
		_surface_map_stroke_before = {}
		_surface_map_stroke_changed = false
		return
	if current_tool == Tool.FOLIAGE_PAINT:
		brush_active = false
		_brush_session.reset_stroke_tracking()
		if _foliage_map_stroke_changed:
			var after_state := _document.capture_foliage_map_history_state()
			_push_foliage_map_history(_foliage_map_stroke_before, after_state)
			is_dirty = true
			_mark_foliage_preview_dirty()
			_mark_ui_state_changed()
		_foliage_map_stroke_before = {}
		_foliage_map_stroke_changed = false
		return
	_brush_session.end_brush_drag(_source_image_for_kind(_brush_session.get_stroke_kind()))


# Public: intersect a viewport-space mouse position with the terrain surface. Returns
# a world-space point, or INVALID_HIT on a miss; pair with is_valid_terrain_hit().
# Used by the Mission workspace to drag/place entities onto the ground.
func raycast_terrain_at(mouse_pos: Vector2) -> Vector3:
	if camera == null or terrain_mesh == null:
		return INVALID_HIT
	return _raycast_terrain_from(mouse_pos)


func is_valid_terrain_hit(hit: Vector3) -> bool:
	return hit != INVALID_HIT


func _raycast_terrain() -> Vector3:
	var mouse_pos := _viewport_mouse_position if _uses_workspace_viewport else get_viewport().get_mouse_position()
	return _raycast_terrain_from(mouse_pos)


func _raycast_terrain_from(mouse_pos: Vector2) -> Vector3:
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
	var result := _brush_session.apply_brush_stroke(delta, _hover_hit, _hover_hit_valid, terrain_mesh, _data)
	if result["changed_heightmap"]:
		terrain_mesh.set_heightmap(_heightmap_image)
		_height_revision += 1
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
		_mark_ui_state_changed()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.TILEINFO:
		var state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_document.restore_tileinfo_history_state(state)
		is_dirty = true
		_mark_tile_overlay_dirty()
		_mark_ui_state_changed()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.FOLIAGEMAP:
		var state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_document.restore_foliage_map_history_state(state)
		is_dirty = true
		_mark_foliage_preview_dirty()
		_mark_ui_state_changed()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.FOLIAGE_DEFS:
		var defs_state: Variant = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		if defs_state is Dictionary:
			_document.restore_foliage_editor_history_state(defs_state)
		else:
			_document.restore_foliage_defs_history_state(defs_state)
		is_dirty = true
		_mark_foliage_preview_dirty()
		_mark_ui_state_changed()
		return
	var result := _brush_session.apply_history_snapshot(snapshot, is_undo, _heightmap_image, _blendmap_image, _colormap_image)
	if result["changed_heightmap"]:
		terrain_mesh.set_heightmap(_heightmap_image)
		_height_revision += 1
		_mark_tile_overlay_dirty()
	if result["changed_blendmap"]:
		_blendmap_tex.update(_blendmap_image)
	if result["changed_colormap"]:
		_colormap_tex.update(_colormap_image)
	if result["changed_heightmap"] or result["changed_blendmap"] or result["changed_colormap"]:
		is_dirty = true


func _source_image_for_kind(kind: int) -> Image:
	match kind:
		TerrainEditHistory.Kind.HEIGHTMAP:
			return _heightmap_image
		TerrainEditHistory.Kind.BLENDMAP:
			return _blendmap_image
		TerrainEditHistory.Kind.COLORMAP:
			return _colormap_image
	return null


func get_ui_state_version() -> int:
	return _ui_state_version


func _mark_ui_state_changed() -> void:
	_ui_state_version += 1
	ui_state_changed.emit(_ui_state_version)
	if workstation and workstation.has_method("sync_from_editor_state"):
		workstation.sync_from_editor_state()


func has_pending_unsaved_action() -> bool:
	return _pending_unsaved_action.is_valid()


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
	if is_export_running():
		return
	_brush_session.clear_history()
	clear_clone_source()
	_create_default_document("untitled")
	_set_heightmap_image(_create_heightmap_image(DEFAULT_HEIGHT))
	_sync_sector_layout(true)
	_mark_foliage_preview_dirty()
	is_dirty = false
	_mark_ui_state_changed()


func open_trn(trn_path: String, timeline: PerfTimeline = null) -> Error:
	if is_export_running():
		return ERR_BUSY
	var resources := get_resource_root()
	if not FileAccess.file_exists(trn_path) and resources != null and resources.has_file(trn_path):
		return _open_trn_from_resource_root(resources, trn_path, timeline)
	_brush_session.clear_history()
	clear_clone_source()
	PerfTimeline.span_on(timeline, "trn_data")
	_data = NovaTerrainData.new()
	_data.set_trn_path(trn_path)
	var err := _data.load()
	if err != OK:
		return err
	PerfTimeline.end_on(timeline)

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
	PerfTimeline.span_on(timeline, "heightmap")
	if FileAccess.file_exists(depth_path):
		var depth_file := FileAccess.open(depth_path, FileAccess.READ)
		if depth_file:
			depth_bytes = depth_file.get_buffer(HM_SIZE * HM_SIZE * 2)
			depth_file.close()
	if depth_bytes.size() == HM_SIZE * HM_SIZE * 2:
		_set_heightmap_image(_build_heightmap_from_raw16(depth_bytes))
	else:
		_set_heightmap_image(_build_heightmap_from_data())
	PerfTimeline.end_on(timeline)

	PerfTimeline.span_on(timeline, "textures")
	_apply_loaded_textures_from_data()
	PerfTimeline.end_on(timeline)
	PerfTimeline.span_on(timeline, "sectors")
	var normalized := _normalize_sector_layout_if_needed()
	_sync_sector_layout(true)
	PerfTimeline.end_on(timeline)
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
	_mark_ui_state_changed()
	_mark_ui_state_changed()
	_check_loaded_cdep_violations()
	return OK


func _open_trn_from_resource_root(resources: NovaResourceRoot, trn_name: String, timeline: PerfTimeline = null) -> Error:
	if resources == null:
		return ERR_INVALID_PARAMETER
	_brush_session.clear_history()
	clear_clone_source()
	PerfTimeline.span_on(timeline, "trn_data")
	_data = NovaTerrainData.new()
	var err := _data.load_from_resource_root(resources, trn_name)
	if err != OK:
		return err
	PerfTimeline.end_on(timeline)

	texture_files = {}
	_apply_default_visual_state(false)

	PerfTimeline.span_on(timeline, "heightmap")
	var depth_bytes := resources.read_file("%s_depth.raw" % _data.get_terrain_name())
	if depth_bytes.size() == HM_SIZE * HM_SIZE * 2:
		_set_heightmap_image(_build_heightmap_from_raw16(depth_bytes))
	else:
		_set_heightmap_image(_build_heightmap_from_data())
	PerfTimeline.end_on(timeline)

	PerfTimeline.span_on(timeline, "textures")
	_apply_loaded_textures_from_data()
	PerfTimeline.end_on(timeline)
	PerfTimeline.span_on(timeline, "sectors")
	var normalized := _normalize_sector_layout_if_needed()
	_sync_sector_layout(true)
	PerfTimeline.end_on(timeline)
	_document.capture_trn_resource(_data)
	var tileinfo := _data.get_tileinfo_resource()
	if tileinfo != null:
		_document.tileinfo_resource = tileinfo
		_document.tileinfo_source_path = resources.get_root_dir().path_join(_data.get_tileinfo_filename())
		_document.tileinfo_state = "explicit"
		_document.tileinfo_selected_index = -1
	var normalized_tileinfo := _normalize_loaded_tileinfo_if_needed()
	_document.current_trn_path = trn_name.get_file()
	_document.current_project_dir = ""
	_remember_open_path(resources.get_root_dir().path_join(trn_name.get_file()))

	is_dirty = normalized or normalized_tileinfo
	_mark_foliage_preview_dirty()
	_mark_ui_state_changed()
	_mark_ui_state_changed()
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

	var raw16 := _data.get_depth_raw16()
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
	_mark_ui_state_changed()
	_mark_ui_state_changed()
	return OK


# Heightmaps loaded from project .trn files can predate the editor's live
# CDEP enforcement (or originate from external tools). If any 256-pixel
# horizontal block exceeds the per-block range limit, prompt the user to
# auto-clamp; otherwise the eventual DFX/JO export will fail at the bake
# guard with a less actionable message.
func _check_loaded_cdep_violations() -> void:
	if _data == null or not _heightmap_image:
		return
	var count := _data.cdep_count_violations()
	if count == 0:
		return
	if workstation and workstation.has_method("prompt_cdep_violations"):
		workstation.prompt_cdep_violations(count, Callable(self, "_auto_fix_cdep_violations"))
	else:
		_notify_status("Heightmap has %d area%s too steep for Joint Operations / DFX export." % [count, "" if count == 1 else "s"])


func _auto_fix_cdep_violations() -> void:
	if _data == null or not _heightmap_image:
		return
	var clamped := _data.cdep_clamp_all_violations()
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
	if _data == null or not _heightmap_image:
		return
	var clamped := _data.cdep_clamp_all_violations()
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
	if flavor == ExportFlavor.DFX_JO and not _document.cdep_ranges_valid(_heightmap_image):
		_notify_status("Export blocked: some areas are too steep for Joint Operations / DFX. Flatten them, or export for original Delta Force instead.")
		return ERR_INVALID_DATA
	var raw16 := _data.get_depth_raw16()
	if raw16.is_empty():
		return ERR_INVALID_DATA
	var builder: NovaTerrainBuilder = NovaTerrainBuilder.new()
	var job: NovaTerrainBuildJob = builder.begin_build_from_data(raw16, output_dir, name, "", flavor)
	if job == null:
		return ERR_CANT_CREATE

	_export_job = job
	_export_output_dir = output_dir
	brush_active = false
	_brush_session.reset_stroke_tracking()
	_remember_export_dir(output_dir)

	if workstation and workstation.has_method("on_export_started"):
		workstation.on_export_started(output_dir)

	return OK


func export_terrain(output_dir: String, flavor: int = ExportFlavor.DFX_JO) -> Error:
	if is_export_running():
		return ERR_BUSY

	DirAccess.make_dir_recursive_absolute(output_dir)
	_auto_clamp_for_export_if_needed(flavor)
	if flavor == ExportFlavor.DFX_JO and not _document.cdep_ranges_valid(_heightmap_image):
		_notify_status("Export blocked: some areas are too steep for Joint Operations / DFX. Flatten them, or export for original Delta Force instead.")
		return ERR_INVALID_DATA
	var raw16 := _data.get_depth_raw16()
	if raw16.is_empty():
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
	_mark_ui_state_changed()
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
	_mark_ui_state_changed()


func _create_default_document(terrain_name: String) -> void:
	_document.create_default_document(terrain_name, _get_material(), _build_default_sector_grid())
	_active_sector_cell = Vector2i(-1, -1)
	selected_surface_index = TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX
	_mark_ui_state_changed()


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


func _set_heightmap_image(image: Image) -> void:
	_document.set_heightmap_image(image)
	terrain_mesh.set_heightmap(_heightmap_image)
	_height_revision += 1
	_mark_foliage_preview_dirty()
	_mark_tile_overlay_dirty()


func _set_colormap_image(image: Image, sync_data: bool = true) -> void:
	_document.set_colormap_image(_get_material(), image, sync_data)


func _set_blendmap_image(image: Image, sync_data: bool = true) -> void:
	_document.set_blendmap_image(_get_material(), image, sync_data)


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


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		request_quit_editor()
